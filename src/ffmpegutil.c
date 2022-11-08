#include "ffmpegutil.h"

static inline bool is_space(int ch) { return ch == '\t' || ch == '\n' || ch == '\r' || ch == ' '; }

char const *ffmpegutil_find_preferred_decoder(char const *const decoders,
                                              char const *const codec,
                                              size_t *const pos,
                                              char *const buf32) {
  size_t const codec_len = strlen(codec);
  while (decoders[*pos] != '\0') {
    char const *token = decoders + *pos;
    if (*token == ',') {
      ++*pos;
      continue;
    }
    char const *sep = strchr(token, ',');
    size_t token_len = sep ? (size_t)(sep - token) : strlen(token);
    *pos += token_len + (sep ? 1 : 0);
    while (token_len > 0 && is_space(*token)) {
      ++token;
      --token_len;
    }
    while (token_len > 0 && is_space(token[token_len - 1])) {
      --token_len;
    }
    if ((token_len < codec_len) || (token_len >= 32)) {
      continue;
    }
    if (strncmp(codec, token, codec_len) != 0) {
      continue;
    }
    if (token_len > codec_len && token[codec_len] != '_') {
      continue;
    }
    strncpy(buf32, token, token_len);
    buf32[token_len] = '\0';
    return buf32;
  }
  return NULL;
}
