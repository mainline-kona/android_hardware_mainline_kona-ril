/*
**
** Copyright 2020, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include "base64util.h"

#include <openssl/base64.h>
#include <string.h>

extern "C" {

int base64_decode(const char *base64input, unsigned char *bindata) {
  if (!base64input || !bindata) {
    return 0;
  }

  size_t in_len = strlen(base64input);
  size_t out_len = 0;
  if (!EVP_DecodedLength(&out_len, in_len)) {
    return 0;
  }
  if (!EVP_DecodeBase64(bindata, &out_len, out_len,
                        reinterpret_cast<const uint8_t *>(base64input), in_len)) {
    return 0;
  }
  return out_len;
}

char *base64_encode(const unsigned char *bindata, char *base64output,
                    int binlength) {
  if (!base64output || !bindata || binlength <= 0) {
    return NULL;
  }

  EVP_EncodeBlock(reinterpret_cast<uint8_t *>(base64output), bindata, binlength);
  return base64output;
}
}
