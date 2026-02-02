/**
 * Renspired - TI-Nspire Gemini Bridge
 *
 * Access LLMs via ESP32 gateway.
 * Press ESC to exit, Up/Down to scroll.
 */

#include <libndls.h>
#include <nspireio/nspireio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * UART Hardware
 * ============================================================================
 */

/* Who woulda guessed TI doesn't make it easy to use this */
#define UART_BASE 0x90020000 /* PL011 location */
#define UART_DR (*(volatile unsigned *)(UART_BASE + 0x00))
#define UART_FR (*(volatile unsigned *)(UART_BASE + 0x18))
#define UART_IBRD (*(volatile unsigned *)(UART_BASE + 0x24))
#define UART_FBRD (*(volatile unsigned *)(UART_BASE + 0x28))
#define UART_LCR_H (*(volatile unsigned *)(UART_BASE + 0x2C))
#define UART_CR (*(volatile unsigned *)(UART_BASE + 0x30))

#define UART_FR_TXFF (1 << 5)
#define UART_FR_TXFE (1 << 7)
#define UART_FR_BUSY (1 << 3)
#define UART_LCR_8BIT (3 << 5)
#define UART_LCR_FEN (1 << 4)
#define UART_CR_UARTEN (1 << 0)
#define UART_CR_TXE (1 << 8)
#define UART_CR_RXE (1 << 9)

#define UART_CLK 12000000
#define BAUD_RATE 115200 /* See ESP32 sketch for baud rate reasoning */

static inline unsigned get_time_ms(void) {
  return (*(volatile unsigned *)0x90090000) * 1000;
}

/* ============================================================================
 * Configuration
 * ============================================================================
 */

#define MAX_INPUT_LEN 256
#define MAX_HISTORY_TURNS 20
#define MAX_RESPONSE_LEN 16384
#define SCROLLBACK_LINES 1000
#define CONSOLE_COLS NIO_MAX_COLS
#define CONSOLE_ROWS NIO_MAX_ROWS
/* Subtract 2 rows for bottom prompt bar. Other 8 work around a bug I don't
 * understand */
#define VISIBLE_LINES (CONSOLE_ROWS - 10)
#define EOT_CHAR 0x04

/* ============================================================================
 * Data Structures
 * ============================================================================
 */

typedef struct {
  char role[8];
  char *content;
} ChatTurn;

typedef struct {
  ChatTurn turns[MAX_HISTORY_TURNS];
  int count;
} ChatHistory;

typedef struct {
  char lines[SCROLLBACK_LINES][CONSOLE_COLS + 1];
  int line_count;
  int scroll_offset;
} ScrollBuffer;

/* ============================================================================
 * Globals
 * ============================================================================
 */

static nio_console csl;
static ChatHistory history;
static ScrollBuffer scrollback;
static char input_buffer[MAX_INPUT_LEN];
static int input_len = 0;
static unsigned os_ibrd, os_fbrd, os_lcr, os_cr;

/* ============================================================================
 * UART Functions
 * ============================================================================
 */

static void uart_init(void) {
  while (!(UART_FR & UART_FR_TXFE))
    ;
  UART_CR = 0;
  while (UART_FR & UART_FR_BUSY)
    ;

  unsigned divisor = (UART_CLK * 4) / BAUD_RATE;
  UART_IBRD = divisor >> 6;
  UART_FBRD = divisor & 0x3F;
  UART_LCR_H = UART_LCR_8BIT | UART_LCR_FEN;
  UART_CR = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
}

static inline bool uart_has_data(void) { return uart_ready(); }
static inline char uart_read_char(void) { return (char)UART_DR; }

static void uart_write_char(char c) {
  while (UART_FR & UART_FR_TXFF)
    ;
  UART_DR = c;
}

static void uart_write_str(const char *s) {
  while (*s)
    uart_write_char(*s++);
}

static void uart_drain(unsigned ms) {
  unsigned start = get_time_ms();
  while ((get_time_ms() - start) < ms) {
    while (uart_has_data())
      uart_read_char();
    idle();
  }
}

/* ============================================================================
 * Handshake
 * ============================================================================
 */

static bool uart_handshake(void) {
  nio_printf("Connecting to ESP32...\n");

  uart_drain(100);

  /* Send wake bytes to wake ESP32 from sleep */
  uart_write_str("\n\n\n\n\n");

  /* Wait for AWAKE response (if ESP32 was sleeping) or ESP_READY (if not) */
  char buf[32];
  int idx = 0;
  unsigned start = get_time_ms();

  while ((get_time_ms() - start) < 2000) {
    if (uart_has_data()) {
      char c = uart_read_char();
      if (c == '\n') {
        buf[idx] = '\0';
        if (strcmp(buf, "AWAKE") == 0 || strcmp(buf, "ESP_READY") == 0) {
          break;
        }
        idx = 0;
      } else if (c != '\r' && idx < 31) {
        buf[idx++] = c;
      }
    }
  }

  uart_write_str("RST\n");

  idx = 0;
  start = get_time_ms();

  /* Wait for ESP_READY */
  while ((get_time_ms() - start) < 15000) {
    if (isKeyPressed(KEY_NSPIRE_ESC))
      return false;

    if (uart_has_data()) {
      char c = uart_read_char();
      if (c == '\n') {
        buf[idx] = '\0';
        if (strcmp(buf, "ESP_READY") == 0)
          break;
        idx = 0;
      } else if (c != '\r' && idx < 31) {
        buf[idx++] = c;
      }
    }
    idle();
  }

  /* Send SYNC and wait for READY */
  uart_drain(50);
  uart_write_str("SYNC\n");

  idx = 0;
  start = get_time_ms();

  while ((get_time_ms() - start) < 5000) {
    if (uart_has_data()) {
      char c = uart_read_char();
      if (c == '\n') {
        buf[idx] = '\0';
        if (strcmp(buf, "READY") == 0) {
          nio_printf("Connected!\n");
          return true;
        }
        idx = 0;
      } else if (c != '\r' && idx < 31) {
        buf[idx++] = c;
      }
    }
    idle();
  }

  nio_printf("Connection failed.\n");
  return false;
}

/* ============================================================================
 * Display Functions
 * ============================================================================
 */

static void scroll_add_line(const char *line) {
  if (scrollback.line_count < SCROLLBACK_LINES) {
    strncpy(scrollback.lines[scrollback.line_count], line, CONSOLE_COLS);
    scrollback.lines[scrollback.line_count][CONSOLE_COLS] = '\0';
    scrollback.line_count++;
  } else {
    memmove(scrollback.lines[0], scrollback.lines[1],
            (SCROLLBACK_LINES - 1) * (CONSOLE_COLS + 1));
    strncpy(scrollback.lines[SCROLLBACK_LINES - 1], line, CONSOLE_COLS);
    scrollback.lines[SCROLLBACK_LINES - 1][CONSOLE_COLS] = '\0';
  }
}

static void scroll_add_text(const char *prefix, const char *text) {
  char line[CONSOLE_COLS + 1];
  int col = 0;

  if (prefix) {
    int plen = strlen(prefix);
    if (plen > CONSOLE_COLS)
      plen = CONSOLE_COLS;
    memcpy(line, prefix, plen);
    col = plen;
  }

  while (*text) {
    if (*text == '\n' || col >= CONSOLE_COLS) {
      line[col] = '\0';
      scroll_add_line(line);
      col = 0;
      if (*text == '\n')
        text++;
    } else {
      line[col++] = *text++;
    }
  }

  if (col > 0) {
    line[col] = '\0';
    scroll_add_line(line);
  }
}

static void redraw(void) {
  /* Don't draw to screen while updating it to prevent flicker */
  nio_drawing_enabled(&csl, false);

  nio_clear(&csl);

  int start = scrollback.line_count - VISIBLE_LINES - scrollback.scroll_offset;
  if (start < 0)
    start = 0;

  for (int i = 0; i < VISIBLE_LINES && (start + i) < scrollback.line_count;
       i++) {
    nio_fputs(scrollback.lines[start + i], &csl);
    nio_fputc('\n', &csl);
  }

  for (int i = 0; i < CONSOLE_COLS; i++)
    nio_fputc('-', &csl);
  nio_fputc('\n', &csl);
  nio_fputs("> ", &csl);
  nio_fputs(input_buffer, &csl);

  /* Flush to screen */
  nio_drawing_enabled(&csl, true);
  nio_fflush(&csl);
}

/* ============================================================================
 * JSON Helpers
 * ============================================================================
 */

static void json_escape_to_uart(const char *s) {
  while (*s) {
    switch (*s) {
    case '"':
      uart_write_str("\\\"");
      break;
    case '\\':
      uart_write_str("\\\\");
      break;
    case '\n':
      uart_write_str("\\n");
      break;
    case '\r':
      uart_write_str("\\r");
      break;
    case '\t':
      uart_write_str("\\t");
      break;
    default:
      if (*s >= 32 && *s < 127)
        uart_write_char(*s);
      break;
    }
    s++;
  }
}

/* Wake ESP32 from light-sleep before sending a request */
static void wake_esp32(void) {
  /* Send wake bytes to trigger ESP32 wake from sleep */
  uart_write_str("\n\n\n");

  /* Brief delay to let ESP32 wake and stabilize UART */
  unsigned start = get_time_ms();
  while ((get_time_ms() - start) < 20) {
    /* Drain any garbage from wakeup */
    if (uart_has_data())
      uart_read_char();
  }
}

static void send_request(const char *prompt) {
  wake_esp32();

  uart_write_str("{\"history\":[");

  for (int i = 0; i < history.count; i++) {
    if (i > 0)
      uart_write_char(',');
    uart_write_str("{\"role\":\"");
    uart_write_str(history.turns[i].role);
    uart_write_str("\",\"parts\":[{\"text\":\"");
    json_escape_to_uart(history.turns[i].content);
    uart_write_str("\"}]}");
  }

  uart_write_str("],\"current_prompt\":\"");
  json_escape_to_uart(prompt);
  uart_write_str("\"}\n");
}

/* ============================================================================
 * Response Handling
 * ============================================================================
 */

static int wait_for_len_or_error(void) {
  /* Wait for either LEN:xxxx or ERR:xxxx */
  char buf[32];
  int idx = 0;
  unsigned start = get_time_ms();

  while ((get_time_ms() - start) < 60000) {
    if (uart_has_data()) {
      char c = uart_read_char();
      if (c == '\n') {
        buf[idx] = '\0';
        if (strncmp(buf, "LEN:", 4) == 0) {
          return atoi(buf + 4); /* Return length */
        }
        if (strncmp(buf, "ERR:", 4) == 0) {
          scroll_add_text("[", buf);
          scroll_add_line("]");
          return -1; /* Error */
        }
        idx = 0; /* Reset for next line */
      } else if (idx < 31) {
        buf[idx++] = c;
      }
    }
    if (isKeyPressed(KEY_NSPIRE_ESC)) {
      scroll_add_line("[Cancelled]");
      return -1;
    }
  }
  scroll_add_line("[Timeout waiting for response]");
  return -1;
}

static bool receive_response(char *response_buf, int max_len) {
  /* Get length from header */
  int expected_len = wait_for_len_or_error();
  if (expected_len < 0) {
    redraw();
    return false;
  }
  if (expected_len == 0) {
    response_buf[0] = '\0';
    scroll_add_line("AI: (empty response)");
    redraw();
    return true;
  }
  if (expected_len >= max_len) {
    expected_len = max_len - 1;
  }

  /* Send ACK for LEN header */
  uart_write_char('A');

  /* Receive data in chunks with ACK */
  int received = 0;
  unsigned start = get_time_ms();
  const int CHUNK_SIZE = 64;

  while (received < expected_len) {
    int chunk_target = (expected_len - received > CHUNK_SIZE)
                           ? CHUNK_SIZE
                           : (expected_len - received);
    int chunk_got = 0;

    /* Read one chunk */
    while (chunk_got < chunk_target && (get_time_ms() - start) < 120000) {
      if (uart_has_data()) {
        char c = uart_read_char();
        if (c == EOT_CHAR) {
          /* Use what we have if EOT comes early */
          goto done;
        }
        response_buf[received + chunk_got] = c;
        chunk_got++;
        start = get_time_ms(); /* Reset timeout */
      } else if (isKeyPressed(KEY_NSPIRE_ESC)) {
        scroll_add_line("[Cancelled]");
        response_buf[received] = '\0';
        redraw();
        return false;
      }
    }

    received += chunk_got;

    /* Send ACK for this chunk */
    uart_write_char('A');
  }

done:
  response_buf[received] = '\0';

  /* Wait for EOT */
  start = get_time_ms();
  while ((get_time_ms() - start) < 2000) {
    if (uart_has_data()) {
      if (uart_read_char() == EOT_CHAR)
        break;
    }
  }

  /* Display the response, scroll to show its beginning */
  int line_before = scrollback.line_count;

  scroll_add_text("AI: ", response_buf);
  scroll_add_line("");

  /* Calculate scroll offset to show start of response at top.
   * From redraw(): start = line_count - VISIBLE_LINES - scroll_offset
   * We want: start = line_before (first line of response)
   * So: scroll_offset = line_count - VISIBLE_LINES - line_before
   *                          = response_lines - VISIBLE_LINES
   * This seems to work
   */
  int response_lines = scrollback.line_count - line_before;
  int target_offset = response_lines - VISIBLE_LINES;

  /* Clamp to valid range */
  int max_offset = scrollback.line_count - VISIBLE_LINES;
  if (max_offset < 0)
    max_offset = 0;
  if (target_offset > max_offset)
    target_offset = max_offset;
  if (target_offset < 0)
    target_offset = 0;

  scrollback.scroll_offset = target_offset;

  redraw();

  return true;
}

/* ============================================================================
 * History Management
 * ============================================================================
 */

static void history_add(const char *role, const char *content) {
  if (history.count >= MAX_HISTORY_TURNS) {
    free(history.turns[0].content);
    memmove(&history.turns[0], &history.turns[1],
            (MAX_HISTORY_TURNS - 1) * sizeof(ChatTurn));
    history.count--;
  }

  strcpy(history.turns[history.count].role, role);
  history.turns[history.count].content = strdup(content);
  history.count++;
}

static void history_free(void) {
  for (int i = 0; i < history.count; i++) {
    free(history.turns[i].content);
  }
  history.count = 0;
}

/* ============================================================================
 * Keyboard
 * ============================================================================
 */

typedef struct {
  const t_key *key;
  char normal;
  char shifted;
} KeyMap;

static const KeyMap key_map[] = {
    {&KEY_NSPIRE_A, 'a', 'A'},      {&KEY_NSPIRE_B, 'b', 'B'},
    {&KEY_NSPIRE_C, 'c', 'C'},      {&KEY_NSPIRE_D, 'd', 'D'},
    {&KEY_NSPIRE_E, 'e', 'E'},      {&KEY_NSPIRE_F, 'f', 'F'},
    {&KEY_NSPIRE_G, 'g', 'G'},      {&KEY_NSPIRE_H, 'h', 'H'},
    {&KEY_NSPIRE_I, 'i', 'I'},      {&KEY_NSPIRE_J, 'j', 'J'},
    {&KEY_NSPIRE_K, 'k', 'K'},      {&KEY_NSPIRE_L, 'l', 'L'},
    {&KEY_NSPIRE_M, 'm', 'M'},      {&KEY_NSPIRE_N, 'n', 'N'},
    {&KEY_NSPIRE_O, 'o', 'O'},      {&KEY_NSPIRE_P, 'p', 'P'},
    {&KEY_NSPIRE_Q, 'q', 'Q'},      {&KEY_NSPIRE_R, 'r', 'R'},
    {&KEY_NSPIRE_S, 's', 'S'},      {&KEY_NSPIRE_T, 't', 'T'},
    {&KEY_NSPIRE_U, 'u', 'U'},      {&KEY_NSPIRE_V, 'v', 'V'},
    {&KEY_NSPIRE_W, 'w', 'W'},      {&KEY_NSPIRE_X, 'x', 'X'},
    {&KEY_NSPIRE_Y, 'y', 'Y'},      {&KEY_NSPIRE_Z, 'z', 'Z'},
    {&KEY_NSPIRE_0, '0', ')'},      {&KEY_NSPIRE_1, '1', '!'},
    {&KEY_NSPIRE_2, '2', '@'},      {&KEY_NSPIRE_3, '3', '#'},
    {&KEY_NSPIRE_4, '4', '$'},      {&KEY_NSPIRE_5, '5', '%'},
    {&KEY_NSPIRE_6, '6', '^'},      {&KEY_NSPIRE_7, '7', '&'},
    {&KEY_NSPIRE_8, '8', '*'},      {&KEY_NSPIRE_9, '9', '('},
    {&KEY_NSPIRE_SPACE, ' ', ' '},  {&KEY_NSPIRE_PERIOD, '.', '>'},
    {&KEY_NSPIRE_COMMA, ',', '<'},  {&KEY_NSPIRE_PLUS, '+', '+'},
    {&KEY_NSPIRE_MINUS, '-', '_'},  {&KEY_NSPIRE_MULTIPLY, '*', '*'},
    {&KEY_NSPIRE_DIVIDE, '/', '?'}, {&KEY_NSPIRE_EQU, '=', '+'},
    {&KEY_NSPIRE_LP, '(', '['},     {&KEY_NSPIRE_RP, ')', ']'},
    {&KEY_NSPIRE_COLON, ':', ';'},  {&KEY_NSPIRE_APOSTROPHE, '\'', '"'},
};

#define KEY_MAP_SIZE (sizeof(key_map) / sizeof(key_map[0]))
static bool key_was_pressed[KEY_MAP_SIZE];

/* ============================================================================
 * Main
 * ============================================================================
 */

int main(void) {
  if (!nio_init(&csl, CONSOLE_COLS, CONSOLE_ROWS, 0, 0, NIO_COLOR_BLACK,
                NIO_COLOR_WHITE, true)) {
    return 1;
  }
  nio_set_default(&csl);

  memset(&history, 0, sizeof(history));
  memset(&scrollback, 0, sizeof(scrollback));
  memset(key_was_pressed, 0, sizeof(key_was_pressed));
  input_buffer[0] = '\0';

  /* Save original UART config */
  os_ibrd = UART_IBRD;
  os_fbrd = UART_FBRD;
  os_lcr = UART_LCR_H;
  os_cr = UART_CR;

  nio_printf("=== Renspired ===\n");
  uart_init();

  bool connected = uart_handshake();
  if (!connected) {
    nio_printf("Press any key to continue offline...\n");
    wait_key_pressed();
  }

  scroll_add_line("=== Renspired ===");
  scroll_add_line("Type and press Enter. ESC to exit.");
  scroll_add_line("");
  redraw();

  bool up_was = false, down_was = false, enter_was = false, del_was = false;

  while (1) {
    if (isKeyPressed(KEY_NSPIRE_ESC))
      break;

    bool shift = isKeyPressed(KEY_NSPIRE_SHIFT);

    /* Scroll up/down */
    bool up = isKeyPressed(KEY_NSPIRE_UP);
    if (up && !up_was &&
        scrollback.scroll_offset < scrollback.line_count - VISIBLE_LINES) {
      scrollback.scroll_offset++;
      redraw();
    }
    up_was = up;

    bool down = isKeyPressed(KEY_NSPIRE_DOWN);
    if (down && !down_was && scrollback.scroll_offset > 0) {
      scrollback.scroll_offset--;
      redraw();
    }
    down_was = down;

    /* Send message */
    bool enter = isKeyPressed(KEY_NSPIRE_ENTER) || isKeyPressed(KEY_NSPIRE_RET);
    if (enter && !enter_was && input_len > 0) {
      scroll_add_text("You: ", input_buffer);
      scroll_add_line("");

      if (connected) {
        history_add("user", input_buffer);
        scroll_add_line("[Thinking...]");
        redraw();

        send_request(input_buffer);

        /* Remove thinking indicator */
        if (scrollback.line_count > 0)
          scrollback.line_count--;

        char *response = malloc(MAX_RESPONSE_LEN);
        if (response) {
          if (receive_response(response, MAX_RESPONSE_LEN) && response[0]) {
            history_add("model", response);
          }
          free(response);
        }
      } else {
        scroll_add_line("[Not connected]");
      }

      input_buffer[0] = '\0';
      input_len = 0;
      redraw();
    }
    enter_was = enter;

    /* Backspace */
    bool del = isKeyPressed(KEY_NSPIRE_DEL);
    if (del && !del_was && input_len > 0) {
      input_buffer[--input_len] = '\0';
      redraw();
    }
    del_was = del;

    /* Regular keys */
    for (unsigned i = 0; i < KEY_MAP_SIZE; i++) {
      bool pressed = isKeyPressed(*key_map[i].key);
      if (pressed && !key_was_pressed[i] && input_len < MAX_INPUT_LEN - 1) {
        input_buffer[input_len++] =
            shift ? key_map[i].shifted : key_map[i].normal;
        input_buffer[input_len] = '\0';
        redraw();
      }
      key_was_pressed[i] = pressed;
    }

    idle();
  }

  history_free();
  nio_printf("\nExiting...\n");
  msleep(300);

  /* Restore UART */
  while (uart_has_data())
    uart_read_char();
  UART_CR = 0;
  UART_IBRD = os_ibrd;
  UART_FBRD = os_fbrd;
  UART_LCR_H = os_lcr;
  UART_CR = os_cr;

  nio_free(&csl);
  return 0;
}
