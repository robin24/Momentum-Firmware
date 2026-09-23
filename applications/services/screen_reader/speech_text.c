/**
 * @file speech_text.c
 * Pronunciation rewriting and chunking for the screen reader's speech. The rules, in the order
 * the code applies them:
 *
 * 1. A one character announcement is named: a letter by its upper case letter (A as "ay"), a
 *    digit by its word, punctuation by its name.
 * 2. Otherwise the text is scanned as tokens: runs of letters, digits, apostrophes between
 *    letters, and periods or hyphens between two alphanumerics.
 * 3. Whole tokens that match a technical term exactly become its pronunciation.
 * 4. A token with periods that is not a decimal number is split at the periods with "dot"
 *    between the parts; a token with hyphens that matched nothing is split at the hyphens.
 * 5. A token that starts with digits and continues with letters is a number and its unit.
 * 6. Digits, optionally with a decimal part, are a number: words up to six digits without a
 *    leading zero, digits one by one otherwise, "point" and digits for the decimal part.
 * 7. Two to five upper case letters and digits with at least one letter are spelled, unless the
 *    token is a real word from the stop list.
 * 8. Other tokens are copied, in pieces of at most 20 characters: the synthesizer never returns
 *    on words of about 30 letters.
 * 9. Between tokens, sentence punctuation is attached to the previous word, a colon becomes a
 *    comma (a space between digits), symbols become words, everything else a single space.
 * 10. Chunks end at a sentence, a comma or a word boundary when possible.
 */
#include "speech_text.h"

#include <stdbool.h>
#include <string.h>

/** The output buffer, its size, and the length written so far. Always terminated. */
typedef struct {
    char* buf;
    size_t size;
    size_t len;
} Out;

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}

static bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}

static bool is_alpha(char c) {
    return is_upper(c) || is_lower(c);
}

static bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

/** Append one character; it is dropped when the buffer is full. */
static void out_char(Out* o, char c) {
    if(o->len + 1 < o->size) o->buf[o->len++] = c;
    o->buf[o->len] = '\0';
}

static void out_str(Out* o, const char* s) {
    while(*s)
        out_char(o, *s++);
}

/** One space between words: none at the start, none after another space. */
static void out_space(Out* o) {
    if(o->len && o->buf[o->len - 1] != ' ') out_char(o, ' ');
}

/** Start a new word. */
static void out_word(Out* o, const char* s) {
    out_space(o);
    out_str(o, s);
}

/** Attach punctuation to the previous word. */
static void out_punct(Out* o, char c) {
    if(o->len && o->buf[o->len - 1] == ' ') o->len--;
    out_char(o, c);
}

static const char* const ones[] = {
    "zero",     "one",     "two",     "three",     "four",     "five",     "six",
    "seven",    "eight",   "nine",    "ten",       "eleven",   "twelve",   "thirteen",
    "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen",
};

static const char* const tens[] = {
    "",
    "",
    "twenty",
    "thirty",
    "forty",
    "fifty",
    "sixty",
    "seventy",
    "eighty",
    "ninety",
};

/** A value below one million in words, without "and": 1234 is one thousand two hundred thirty
 * four. */
static void number_words(Out* o, unsigned v) {
    if(v >= 1000) {
        number_words(o, v / 1000);
        out_word(o, "thousand");
        v %= 1000;
        if(v == 0) return;
    }
    if(v >= 100) {
        out_word(o, ones[v / 100]);
        out_word(o, "hundred");
        v %= 100;
        if(v == 0) return;
    }
    if(v >= 20) {
        out_word(o, tens[v / 10]);
        v %= 10;
        if(v == 0) return;
    }
    out_word(o, ones[v]);
}

static void digits_one_by_one(Out* o, const char* s, size_t n) {
    for(size_t i = 0; i < n; i++)
        out_word(o, ones[(size_t)(s[i] - '0')]);
}

/** Rule 6: digits, optionally followed by a period and more digits. */
static void number_token(Out* o, const char* s, size_t n) {
    size_t int_len = 0;
    while(int_len < n && is_digit(s[int_len]))
        int_len++;
    bool leading_zero = int_len > 1 && s[0] == '0';
    if(int_len <= 6 && !leading_zero) {
        unsigned v = 0;
        for(size_t i = 0; i < int_len; i++)
            v = v * 10 + (unsigned)(s[i] - '0');
        number_words(o, v);
    } else {
        digits_one_by_one(o, s, int_len);
    }
    if(int_len < n && s[int_len] == '.') {
        out_word(o, "point");
        digits_one_by_one(o, s + int_len + 1, n - int_len - 1);
    }
}

typedef struct {
    const char* from;
    const char* to;
} Term;

/** Rule 3: whole token replacements, exact match. */
static const Term terms[] = {
    {"Sub-GHz", "sub gigahertz"},
    {"SubGHz", "sub gigahertz"},
    {"kHz", "kilohertz"},
    {"MHz", "megahertz"},
    {"GHz", "gigahertz"},
    {"Hz", "hertz"},
    {"dBm", "dee bee em"},
    {"dB", "dee bee"},
    {"iButton", "eye button"},
    {"OK", "okay"},
    {"Ok", "okay"},
    {"PIN", "pin"},
    {"Wi-Fi", "why fye"},
    {"WiFi", "why fye"},
    {"mAh", "milliamp hours"},
    {"MNTM", "momentum"},
};

/** Rule 7: upper case words that are real words, read as words rather than spelled. */
static const char* const stop_words[] = {
    "ON",    "OFF",   "NO",   "YES",  "NEW",  "ADD",   "ALL",  "SET",   "RUN",   "SAVE",  "SEND",
    "READ",  "EXIT",  "BACK", "MENU", "INFO", "TEST",  "FILE", "EDIT",  "OPEN",  "LOCK",  "DEMO",
    "HOLD",  "PLAY",  "STOP", "TIME", "DATE", "MODE",  "AUTO", "EASY",  "NONE",  "MAX",   "MIN",
    "RAW",   "WARN",  "FAIL", "DONE", "LOW",  "HIGH",  "FULL", "START", "RESET", "CLEAR", "ENTER",
    "EMPTY", "RETRY", "MORE", "FREE", "UP",   "DOWN",  "LEFT", "RIGHT", "HOME",  "AND",   "OR",
    "NOT",   "THE",   "GO",   "BAD",  "KEY",  "CARD",  "TAG",  "APP",   "APPS",  "TAB",   "SUB",
    "WAIT",  "SCAN",  "SKIP", "NEXT", "PREV", "ERROR", "OKAY", "LOAD",  "COPY",  "MOVE",  "NAME",
    "TYPE",  "SIZE",  "PAGE", "LIST", "VIEW", "ZERO",  "TEXT", "SAM",
};

static bool token_equals(const char* s, size_t n, const char* word) {
    return strlen(word) == n && strncmp(s, word, n) == 0;
}

static bool is_stop_word(const char* s, size_t n) {
    for(size_t i = 0; i < sizeof(stop_words) / sizeof(stop_words[0]); i++) {
        if(token_equals(s, n, stop_words[i])) return true;
    }
    return false;
}

/** Rule 7: two to five upper case letters and digits, at least one letter, not a stop word. */
static bool is_acronym(const char* s, size_t n) {
    if(n < 2 || n > 5) return false;
    bool has_letter = false;
    for(size_t i = 0; i < n; i++) {
        if(is_upper(s[i])) {
            has_letter = true;
        } else if(!is_digit(s[i])) {
            return false;
        }
    }
    return has_letter && !is_stop_word(s, n);
}

/** Characters separated by spaces, A as "ay" so it is not read as the article. */
static void spell(Out* o, const char* s, size_t n) {
    for(size_t i = 0; i < n; i++) {
        char letter[2] = {s[i], '\0'};
        out_word(o, s[i] == 'A' ? "ay" : letter);
    }
}

/** Rule 6's shape: digits, optionally a period and at least one more digit. */
static bool is_number_token(const char* s, size_t n) {
    size_t i = 0;
    while(i < n && is_digit(s[i]))
        i++;
    if(i == 0) return false;
    if(i == n) return true;
    if(s[i] != '.') return false;
    i++;
    if(i == n) return false;
    while(i < n) {
        if(!is_digit(s[i])) return false;
        i++;
    }
    return true;
}

static void process_token(Out* o, const char* s, size_t n);

/** Rules 5, 7 and 8 for a token without periods or hyphens that is not a term or a number. */
static void process_word(Out* o, const char* s, size_t n) {
    // Rule 5: digits followed by letters are a number and its unit
    size_t d = 0;
    while(d < n && is_digit(s[d]))
        d++;
    if(d > 0 && d < n && is_alpha(s[d])) {
        number_token(o, s, d);
        process_token(o, s + d, n - d);
        return;
    }
    if(is_acronym(s, n)) {
        spell(o, s, n);
        return;
    }
    // Rule 8: pieces of at most 20 characters
    while(n > 0) {
        char piece[21];
        size_t m = n < 20 ? n : 20;
        memcpy(piece, s, m);
        piece[m] = '\0';
        out_word(o, piece);
        s += m;
        n -= m;
    }
}

/** Rule 4: split at every `sep`, each part processed as a token, `between` (may be null)
 * spoken between the parts. */
static void split_token(Out* o, const char* s, size_t n, char sep, const char* between) {
    size_t start = 0;
    bool first = true;
    for(size_t i = 0; i <= n; i++) {
        if(i < n && s[i] != sep) continue;
        if(!first && between) out_word(o, between);
        process_token(o, s + start, i - start);
        first = false;
        start = i + 1;
    }
}

static void process_token(Out* o, const char* s, size_t n) {
    if(n == 0) return;
    // Rule 3
    for(size_t i = 0; i < sizeof(terms) / sizeof(terms[0]); i++) {
        if(token_equals(s, n, terms[i].from)) {
            out_word(o, terms[i].to);
            return;
        }
    }
    // Rule 6
    if(is_number_token(s, n)) {
        number_token(o, s, n);
        return;
    }
    // Rule 4: periods that are not a decimal point, then hyphens that matched nothing
    if(memchr(s, '.', n)) {
        split_token(o, s, n, '.', "dot");
        return;
    }
    if(memchr(s, '-', n)) {
        split_token(o, s, n, '-', NULL);
        return;
    }
    process_word(o, s, n);
}

typedef struct {
    char c;
    const char* name;
} CharName;

/** Rule 1: names of one character announcements. */
static const CharName char_names[] = {
    {' ', "space"},         {'.', "dot"},
    {',', "comma"},         {'-', "dash"},
    {'_', "underscore"},    {'/', "slash"},
    {'\\', "backslash"},    {':', "colon"},
    {';', "semicolon"},     {'!', "exclamation mark"},
    {'?', "question mark"}, {'\'', "apostrophe"},
    {'"', "quote"},         {'+', "plus"},
    {'*', "star"},          {'=', "equals"},
    {'%', "percent"},       {'&', "and"},
    {'#', "number"},        {'@', "at"},
    {'(', "open paren"},    {')', "close paren"},
    {'[', "open bracket"},  {']', "close bracket"},
    {'{', "open brace"},    {'}', "close brace"},
    {'<', "less than"},     {'>', "greater than"},
    {'^', "caret"},         {'~', "tilde"},
    {'`', "backtick"},      {'|', "bar"},
    {'$', "dollar"},
};

/** Rule 1. Returns false when `in` is not exactly one character, or one without a name. */
static bool single_character(const char* in, Out* o) {
    if(in[0] == '\0' || in[1] != '\0') return false;
    char c = in[0];
    if(is_alpha(c)) {
        if(c == 'a' || c == 'A') {
            out_str(o, "ay");
        } else {
            out_char(o, is_lower(c) ? (char)(c - 'a' + 'A') : c);
        }
        return true;
    }
    if(is_digit(c)) {
        out_str(o, ones[(size_t)(c - '0')]);
        return true;
    }
    for(size_t i = 0; i < sizeof(char_names) / sizeof(char_names[0]); i++) {
        if(char_names[i].c == c) {
            out_str(o, char_names[i].name);
            return true;
        }
    }
    return false;
}

/** Rule 2: whether s[i] belongs to a token. */
static bool in_token(const char* s, size_t i, size_t n) {
    char c = s[i];
    if(is_alnum(c)) return true;
    bool prev = i > 0 && is_alnum(s[i - 1]);
    bool next = i + 1 < n && is_alnum(s[i + 1]);
    if(c == '\'') return i > 0 && is_alpha(s[i - 1]) && next && is_alpha(s[i + 1]);
    if(c == '.' || c == '-') return prev && next;
    return false;
}

size_t speech_text_expand(const char* in, char* out, size_t out_size) {
    if(out_size == 0) return 0;
    Out o = {.buf = out, .size = out_size, .len = 0};
    out[0] = '\0';
    if(!in) return 0;
    if(single_character(in, &o)) return o.len;

    size_t n = strlen(in);
    size_t i = 0;
    while(i < n) {
        if(in_token(in, i, n)) {
            size_t start = i;
            while(i < n && in_token(in, i, n))
                i++;
            process_token(&o, in + start, i - start);
            continue;
        }
        // Rule 9: between tokens
        char c = in[i];
        if(c == '.' || c == ',' || c == '?' || c == '!') {
            out_punct(&o, c);
        } else if(c == ':') {
            bool between_digits = i > 0 && i + 1 < n && is_digit(in[i - 1]) && is_digit(in[i + 1]);
            if(between_digits) {
                out_space(&o);
            } else {
                out_punct(&o, ',');
            }
        } else if(c == '/') {
            out_word(&o, "slash");
        } else if(c == '%') {
            out_word(&o, "percent");
        } else if(c == '&') {
            out_word(&o, "and");
        } else if(c == '+') {
            out_word(&o, "plus");
        } else if(c == '=') {
            out_word(&o, "equals");
        } else if(c == '#') {
            out_word(&o, "number");
        } else if(c == '@') {
            out_word(&o, "at");
        } else if(c == '*') {
            out_word(&o, "star");
        } else {
            // whitespace, underscore, quotes, brackets, bars, angle brackets, the remaining
            // printable characters, and every byte outside printable ASCII
            out_space(&o);
        }
        i++;
    }
    while(o.len && o.buf[o.len - 1] == ' ')
        o.buf[--o.len] = '\0';
    return o.len;
}

size_t speech_text_next_chunk(const char* text, size_t* pos, char* chunk, size_t chunk_size) {
    if(chunk_size == 0) return 0;
    chunk[0] = '\0';
    if(!text) return 0;
    size_t len = strlen(text);
    size_t start = *pos;
    while(start < len && text[start] == ' ')
        start++;
    if(start >= len) {
        *pos = len;
        return 0;
    }
    size_t max = SPEECH_CHUNK_MAX;
    if(max > chunk_size - 1) max = chunk_size - 1;
    size_t remaining = len - start;
    size_t take = remaining;
    if(remaining > max) {
        // The last sentence end within the limit, else the last comma, else the last space.
        // text[i + 1] is inside the text: the limit ends before the text does.
        size_t best = 0;
        for(size_t i = start; i < start + max; i++) {
            char c = text[i];
            if((c == '.' || c == '?' || c == '!') && text[i + 1] == ' ') best = i + 1 - start;
        }
        if(!best) {
            for(size_t i = start; i < start + max; i++) {
                if(text[i] == ',' && text[i + 1] == ' ') best = i + 1 - start;
            }
        }
        if(!best) {
            for(size_t i = start; i < start + max; i++) {
                if(text[i] == ' ') best = i - start;
            }
        }
        take = best ? best : max;
    }
    memcpy(chunk, text + start, take);
    chunk[take] = '\0';
    *pos = start + take;
    while(take && chunk[take - 1] == ' ')
        chunk[--take] = '\0';
    return take;
}
