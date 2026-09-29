/**
 * @file speech_text.c
 * Pronunciation rewriting for the screen reader's speech. The rule numbers are the plan's; a
 * token goes through rules 3, 6, 5, 4 with 12, 7, 13 and 8 in that order.
 *
 * 1. A one character announcement is named: a letter by its upper case letter (A as "ay"), a
 *    digit by its word, punctuation by its name.
 * 2. Otherwise the text is scanned as tokens: runs of letters, digits, apostrophes between
 *    letters, and periods or hyphens between two alphanumerics.
 * 3. Whole tokens that match a technical term exactly become its pronunciation.
 * 4. A token with periods that is not a decimal number is split at the periods with "dot"
 *    between the parts; a token with hyphens that matched nothing is split at the hyphens.
 * 5. A number followed by letters is the number and its unit: the letters as a term, spelled
 *    when one to five upper case letters outside the stop list, copied otherwise.
 * 6. Digits, optionally with a decimal part, are a number: words up to four digits without a
 *    leading zero, digits one by one otherwise, "point" and digits for the decimal part.
 * 7. Upper case letters and digits, two or more with at least one letter, outside the stop list,
 *    are spelled by runs: one to five letters character by character unless the run is a stop
 *    word, longer letter runs as a word, one to three digits as a number, longer digit runs
 *    digit by digit.
 * 8. Other tokens are copied, in pieces of at most 20 characters, each short enough for a clip
 *    name.
 * 9. Between tokens, sentence punctuation is attached to the previous word, a colon becomes a
 *    comma (a space between digits), symbols become words, everything else a single space.
 * 10. Retired: the text is no longer cut into chunks, the clips play it word by word.
 * 11. A hyphen before a digit that is not inside a token is "minus".
 * 12. A hyphen with a digit on at least one side: "to" as the only hyphen between two digit runs
 *    of the same length, a space between a digit run and two or more letters, "dash" otherwise.
 * 13. A token that is the single upper case letter A is "ay".
 * 14. A slash between two digits is "of", as in the Passport's points: 18/100 is "eighteen of one
 *    hundred". Any other slash is "slash".
 */
#include "speech_text.h"

#include <stdbool.h>
#include <string.h>

/** The output buffer, its size, and the length written so far. Always terminated. */
typedef struct {
    char* buf;
    size_t size;
    size_t len;
    bool full; /**< a character did not fit */
    bool cut_in_word; /**< the first one that did not fit went on the word the text ends in */
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

/** A character of a word in the expansion: "don't" keeps its apostrophe */
static bool is_word_char(char c) {
    return is_alnum(c) || c == '\'';
}

/** Append one character; it is dropped when the buffer is full. */
static void out_char(Out* o, char c) {
    if(o->len + 1 < o->size) {
        o->buf[o->len++] = c;
    } else if(!o->full) {
        o->full = true;
        o->cut_in_word = is_word_char(c) && o->len && is_word_char(o->buf[o->len - 1]);
    }
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

/** Attach punctuation to the previous word. Once the buffer is full nothing more is written:
 *  taking a space back would let punctuation from further on onto the last word. */
static void out_punct(Out* o, char c) {
    if(o->full) return;
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

/** A value below ten thousand in words, without "and": 1234 is one thousand two hundred thirty
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
    if(int_len <= 4 && !leading_zero) {
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
    {"XP", "experience"},
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
    "TYPE",  "SIZE",  "PAGE", "LIST", "VIEW", "ZERO",  "TEXT", "SAM",   "AT",    "IN",    "OF",
    "TO",    "IS",    "AS",   "BY",   "IT",   "AN",    "BE",   "DO",    "IF",    "SO",    "WE",
    "FOR",   "NOW",   "END",  "TOP",  "BIT",  "HEX",   "BIN",  "DEC",
};

static bool token_equals(const char* s, size_t n, const char* word) {
    return strlen(word) == n && strncmp(s, word, n) == 0;
}

/** Rule 3: speak the replacement of an exact term match. Returns false when there is none. */
static bool out_term(Out* o, const char* s, size_t n) {
    for(size_t i = 0; i < sizeof(terms) / sizeof(terms[0]); i++) {
        if(token_equals(s, n, terms[i].from)) {
            out_word(o, terms[i].to);
            return true;
        }
    }
    return false;
}

static bool is_stop_word(const char* s, size_t n) {
    for(size_t i = 0; i < sizeof(stop_words) / sizeof(stop_words[0]); i++) {
        if(token_equals(s, n, stop_words[i])) return true;
    }
    return false;
}

static bool all_upper(const char* s, size_t n) {
    for(size_t i = 0; i < n; i++) {
        if(!is_upper(s[i])) return false;
    }
    return true;
}

/** Rule 7's shape: upper case letters and digits, two or more characters, at least one letter,
 * the whole token not a stop word. */
static bool is_acronym(const char* s, size_t n) {
    if(n < 2) return false;
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

/** Rule 8: copy in pieces of at most 20 characters. */
static void copy_pieces(Out* o, const char* s, size_t n) {
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

/** Rule 7: spell by runs. A run of one to five letters is spelled unless it is a stop word,
 * a longer letter run is copied as a word; a run of one to three digits is a number, a longer
 * digit run is spoken digit by digit. */
static void spell_by_runs(Out* o, const char* s, size_t n) {
    size_t i = 0;
    while(i < n) {
        size_t start = i;
        bool digits = is_digit(s[i]);
        while(i < n && is_digit(s[i]) == digits)
            i++;
        size_t len = i - start;
        if(digits) {
            if(len <= 3) {
                number_token(o, s + start, len);
            } else {
                digits_one_by_one(o, s + start, len);
            }
        } else if(len <= 5 && !is_stop_word(s + start, len)) {
            spell(o, s + start, len);
        } else {
            copy_pieces(o, s + start, len);
        }
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

/** Rule 5's shape: a number (digits, optionally a period and more digits) followed by letters
 * and nothing else. Returns the number's length, 0 when the token is not shaped like that. */
static size_t number_before_letters(const char* s, size_t n) {
    size_t i = 0;
    while(i < n && is_digit(s[i]))
        i++;
    if(i == 0) return 0;
    if(i + 1 < n && s[i] == '.' && is_digit(s[i + 1])) {
        i++;
        while(i < n && is_digit(s[i]))
            i++;
    }
    if(i == n) return 0;
    for(size_t j = i; j < n; j++) {
        if(!is_alpha(s[j])) return 0;
    }
    return i;
}

/** Rule 5: the letters after a number. */
static void process_unit(Out* o, const char* s, size_t n) {
    if(out_term(o, s, n)) return;
    if(n <= 5 && all_upper(s, n) && !is_stop_word(s, n)) {
        spell(o, s, n);
        return;
    }
    copy_pieces(o, s, n);
}

static void process_token(Out* o, const char* s, size_t n);

/** Letters from s[i] on, as a count. */
static size_t letters_from(const char* s, size_t n, size_t i) {
    size_t k = 0;
    while(i + k < n && is_alpha(s[i + k]))
        k++;
    return k;
}

/** Letters ending just before s[i], as a count. */
static size_t letters_before(const char* s, size_t i) {
    size_t k = 0;
    while(k < i && is_alpha(s[i - 1 - k]))
        k++;
    return k;
}

/** Rule 12: the word for the hyphen at s[i] of a token with `count` hyphens, or null when the
 * parts simply follow each other as in rule 4. `digit_parts` says every part is a digit run. */
static const char* hyphen_word(const char* s, size_t n, size_t i, size_t count, bool digit_parts) {
    bool left_digit = i > 0 && is_digit(s[i - 1]);
    bool right_digit = i + 1 < n && is_digit(s[i + 1]);
    if(!left_digit && !right_digit) return NULL;
    if(digit_parts) {
        // "to" only as the only hyphen, between two runs of the same digit count
        return (count == 1 && i == n - 1 - i) ? "to" : "dash";
    }
    // a digit run next to two or more letters reads as two words
    if(left_digit && letters_from(s, n, i + 1) >= 2) return NULL;
    if(right_digit && letters_before(s, i) >= 2) return NULL;
    return "dash";
}

/** Rules 4 and 12: split at every `sep` and process each part as a token. Periods get "dot"
 * between the parts; hyphens get what hyphen_word says. */
static void split_token(Out* o, const char* s, size_t n, char sep) {
    size_t count = 0;
    bool digit_parts = true;
    for(size_t i = 0; i < n; i++) {
        if(s[i] == sep) {
            count++;
        } else if(!is_digit(s[i])) {
            digit_parts = false;
        }
    }
    size_t start = 0;
    for(size_t i = 0; i <= n; i++) {
        if(i < n && s[i] != sep) continue;
        process_token(o, s + start, i - start);
        if(i < n) {
            const char* between = sep == '.' ? "dot" : hyphen_word(s, n, i, count, digit_parts);
            if(between) out_word(o, between);
        }
        start = i + 1;
    }
}

static void process_token(Out* o, const char* s, size_t n) {
    if(n == 0) return;
    // Rule 3
    if(out_term(o, s, n)) return;
    // Rule 6
    if(is_number_token(s, n)) {
        number_token(o, s, n);
        return;
    }
    // Rule 5
    size_t number = number_before_letters(s, n);
    if(number) {
        number_token(o, s, number);
        process_unit(o, s + number, n - number);
        return;
    }
    // Rule 4: periods that are not a decimal point, then hyphens (rule 12 for digits)
    if(memchr(s, '.', n)) {
        split_token(o, s, n, '.');
        return;
    }
    if(memchr(s, '-', n)) {
        split_token(o, s, n, '-');
        return;
    }
    // Rule 7
    if(is_acronym(s, n)) {
        spell_by_runs(o, s, n);
        return;
    }
    // Rule 13
    if(n == 1 && s[0] == 'A') {
        out_word(o, "ay");
        return;
    }
    // Rule 8
    copy_pieces(o, s, n);
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
        // Rules 9, 11 and 14: between tokens
        char c = in[i];
        bool between_digits = i > 0 && i + 1 < n && is_digit(in[i - 1]) && is_digit(in[i + 1]);
        if(c == '.' || c == ',' || c == '?' || c == '!') {
            out_punct(&o, c);
        } else if(c == ':') {
            if(between_digits) {
                out_space(&o);
            } else {
                out_punct(&o, ',');
            }
        } else if(c == '-' && i + 1 < n && is_digit(in[i + 1])) {
            // Rule 11. The hyphen is outside every token although a digit follows, so it is at
            // the start of the text or after a character that is not alphanumeric.
            out_word(&o, "minus");
        } else if(c == '/') {
            // Rule 14
            out_word(&o, between_digits ? "of" : "slash");
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
    // Full inside a word: the text ends before it. A fragment ("seven se") would be spelled,
    // logged as a missing word, and made into a clip of its own by --pull-missing
    if(o.cut_in_word) {
        while(o.len && is_word_char(o.buf[o.len - 1]))
            o.len--;
        o.buf[o.len] = '\0';
    }
    while(o.len && o.buf[o.len - 1] == ' ')
        o.buf[--o.len] = '\0';
    return o.len;
}

size_t speech_text_spell_copy(const char* in, char* out, size_t out_size) {
    if(out_size == 0) return 0;
    size_t o = 0;
    size_t run = 0;
    bool gap = false; // a separator came after the last character kept
    for(const char* p = in ? in : ""; *p; p++) {
        if(!is_alnum(*p)) {
            gap = true;
            continue;
        }
        if(run == SPEECH_TEXT_SPELL_RUN_MAX) gap = true;
        if(gap && o > 0) {
            if(o + 2 >= out_size) break; // the space and a character, or neither
            out[o++] = ' ';
            run = 0;
        }
        gap = false;
        if(o + 1 >= out_size) break;
        out[o++] = *p;
        run++;
    }
    out[o] = '\0';
    return o;
}
