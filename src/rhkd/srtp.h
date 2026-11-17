// SRTP/SRTCP AES_CM_128_HMAC_SHA1_80 (RFC 3711), sender side only.
// SPDX-License-Identifier: Apache-2.0
#ifndef RHKD_SRTP_H
#define RHKD_SRTP_H
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

#define SRTP_AUTH_TAG_LEN 10
#define SRTCP_TRAILER_LEN (4 + SRTP_AUTH_TAG_LEN)

typedef struct {
    mbedtls_aes_context aes;      // session encryption key
    mbedtls_md_context_t hmac;    // session auth key
    uint8_t salt[14];             // session salt
} srtp_keys_t;

typedef struct {
    srtp_keys_t rtp;
    srtp_keys_t rtcp;
    uint32_t roc;          // rollover counter
    uint16_t lastSeq;
    int haveSeq;
    uint32_t srtcpIndex;
} srtp_ctx_t;

int srtp_init(srtp_ctx_t* c, const uint8_t masterKey[16], const uint8_t masterSalt[14]);
void srtp_free(srtp_ctx_t* c);
// Protects an RTP packet in place. @buf must have SRTP_AUTH_TAG_LEN spare bytes. Returns new length.
size_t srtp_protect_rtp(srtp_ctx_t* c, uint8_t* buf, size_t len);
// Protects an RTCP compound packet in place. @buf must have SRTCP_TRAILER_LEN spare bytes. Returns new length.
size_t srtp_protect_rtcp(srtp_ctx_t* c, uint8_t* buf, size_t len);
// Verifies and decrypts an incoming SRTCP packet in place. Returns the plain RTCP length, or 0 if invalid.
size_t srtp_unprotect_rtcp(srtp_ctx_t* c, uint8_t* buf, size_t len);
#endif
