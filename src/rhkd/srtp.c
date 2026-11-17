// SPDX-License-Identifier: Apache-2.0
#include "srtp.h"
#include <string.h>

// AES-CM keystream XOR: IV is 16 bytes, counter in the last 16 bits.
static void aes_cm(mbedtls_aes_context* aes, const uint8_t iv[16], uint8_t* data, size_t n) {
    uint8_t nonce[16], block[16];
    size_t off = 0;
    memcpy(nonce, iv, 16);
    mbedtls_aes_crypt_ctr(aes, n, &off, nonce, block, data, data);
}

// RFC 3711 4.3.1 key derivation with kdr = 0 (index DIV kdr = 0).
static void derive(const uint8_t mk[16], const uint8_t ms[14], uint8_t label, uint8_t* out, size_t n) {
    mbedtls_aes_context aes;
    uint8_t iv[16] = { 0 };
    memcpy(iv, ms, 14);
    iv[7] ^= label;
    memset(out, 0, n);
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, mk, 128);
    aes_cm(&aes, iv, out, n);
    mbedtls_aes_free(&aes);
}

static int keys_init(srtp_keys_t* k, const uint8_t mk[16], const uint8_t ms[14], uint8_t base) {
    uint8_t ke[16], ka[20];
    derive(mk, ms, base + 0, ke, sizeof ke);
    derive(mk, ms, base + 1, ka, sizeof ka);
    derive(mk, ms, base + 2, k->salt, sizeof k->salt);
    mbedtls_aes_init(&k->aes);
    if (mbedtls_aes_setkey_enc(&k->aes, ke, 128))
        return -1;
    mbedtls_md_init(&k->hmac);
    if (mbedtls_md_setup(&k->hmac, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1))
        return -1;
    if (mbedtls_md_hmac_starts(&k->hmac, ka, sizeof ka))
        return -1;
    memset(ke, 0, sizeof ke);
    memset(ka, 0, sizeof ka);
    return 0;
}

static void keys_free(srtp_keys_t* k) {
    mbedtls_aes_free(&k->aes);
    mbedtls_md_free(&k->hmac);
}

int srtp_init(srtp_ctx_t* c, const uint8_t masterKey[16], const uint8_t masterSalt[14]) {
    memset(c, 0, sizeof *c);
    if (keys_init(&c->rtp, masterKey, masterSalt, 0))
        return -1;
    if (keys_init(&c->rtcp, masterKey, masterSalt, 3))
        return -1;
    return 0;
}

void srtp_free(srtp_ctx_t* c) {
    keys_free(&c->rtp);
    keys_free(&c->rtcp);
}

static void make_iv(uint8_t iv[16], const uint8_t salt[14], uint32_t ssrc, uint64_t index) {
    memset(iv, 0, 16);
    memcpy(iv, salt, 14);
    iv[4] ^= (uint8_t)(ssrc >> 24);
    iv[5] ^= (uint8_t)(ssrc >> 16);
    iv[6] ^= (uint8_t)(ssrc >> 8);
    iv[7] ^= (uint8_t) ssrc;
    for (int i = 0; i < 6; i++)
        iv[13 - i] ^= (uint8_t)(index >> (8 * i));
}

static void auth(srtp_keys_t* k, const uint8_t* d, size_t n, const uint8_t* extra, size_t en, uint8_t* tag) {
    uint8_t full[20];
    mbedtls_md_hmac_reset(&k->hmac);
    mbedtls_md_hmac_update(&k->hmac, d, n);
    if (en)
        mbedtls_md_hmac_update(&k->hmac, extra, en);
    mbedtls_md_hmac_finish(&k->hmac, full);
    memcpy(tag, full, SRTP_AUTH_TAG_LEN);
}

size_t srtp_protect_rtp(srtp_ctx_t* c, uint8_t* buf, size_t len) {
    if (len < 12)
        return 0;
    size_t hdr = 12 + 4 * (buf[0] & 0x0F);
    if (buf[0] & 0x10) { // header extension
        if (len < hdr + 4)
            return 0;
        hdr += 4 + 4 * ((buf[hdr + 2] << 8) | buf[hdr + 3]);
    }
    if (len < hdr)
        return 0;
    uint16_t seq = (uint16_t)((buf[2] << 8) | buf[3]);
    uint32_t ssrc = ((uint32_t) buf[8] << 24) | ((uint32_t) buf[9] << 16) | ((uint32_t) buf[10] << 8) | buf[11];
    // Sender: sequence numbers only move forward, so a wrap bumps the ROC.
    if (c->haveSeq && seq < c->lastSeq && (c->lastSeq - seq) > 0x8000)
        c->roc++;
    c->lastSeq = seq;
    c->haveSeq = 1;
    uint64_t index = ((uint64_t) c->roc << 16) | seq;

    uint8_t iv[16];
    make_iv(iv, c->rtp.salt, ssrc, index);
    aes_cm(&c->rtp.aes, iv, buf + hdr, len - hdr);

    uint8_t roc[4] = { (uint8_t)(c->roc >> 24), (uint8_t)(c->roc >> 16), (uint8_t)(c->roc >> 8), (uint8_t) c->roc };
    auth(&c->rtp, buf, len, roc, 4, buf + len);
    return len + SRTP_AUTH_TAG_LEN;
}

size_t srtp_protect_rtcp(srtp_ctx_t* c, uint8_t* buf, size_t len) {
    if (len < 8)
        return 0;
    uint32_t ssrc = ((uint32_t) buf[4] << 24) | ((uint32_t) buf[5] << 16) | ((uint32_t) buf[6] << 8) | buf[7];
    uint32_t index = c->srtcpIndex++ & 0x7FFFFFFF;
    uint8_t iv[16];
    make_iv(iv, c->rtcp.salt, ssrc, index);
    aes_cm(&c->rtcp.aes, iv, buf + 8, len - 8);
    uint32_t e = 0x80000000u | index;
    buf[len + 0] = (uint8_t)(e >> 24);
    buf[len + 1] = (uint8_t)(e >> 16);
    buf[len + 2] = (uint8_t)(e >> 8);
    buf[len + 3] = (uint8_t) e;
    len += 4;
    auth(&c->rtcp, buf, len, NULL, 0, buf + len);
    return len + SRTP_AUTH_TAG_LEN;
}

size_t srtp_unprotect_rtcp(srtp_ctx_t* c, uint8_t* buf, size_t len) {
    if (len < 8 + SRTCP_TRAILER_LEN)
        return 0;
    size_t authLen = len - SRTP_AUTH_TAG_LEN;
    uint8_t tag[SRTP_AUTH_TAG_LEN];
    auth(&c->rtcp, buf, authLen, NULL, 0, tag);
    uint8_t diff = 0;
    for (size_t i = 0; i < SRTP_AUTH_TAG_LEN; i++)
        diff |= (uint8_t)(tag[i] ^ buf[authLen + i]);
    if (diff)
        return 0;
    size_t plainLen = authLen - 4;
    uint32_t e = ((uint32_t) buf[plainLen] << 24) | ((uint32_t) buf[plainLen + 1] << 16) |
                 ((uint32_t) buf[plainLen + 2] << 8) | buf[plainLen + 3];
    if (e & 0x80000000u) {
        uint32_t ssrc = ((uint32_t) buf[4] << 24) | ((uint32_t) buf[5] << 16) | ((uint32_t) buf[6] << 8) | buf[7];
        uint8_t iv[16];
        make_iv(iv, c->rtcp.salt, ssrc, e & 0x7FFFFFFF);
        aes_cm(&c->rtcp.aes, iv, buf + 8, plainLen - 8);
    }
    return plainLen;
}
