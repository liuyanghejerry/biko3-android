/* CommonCrypto SHA-256 compatibility for the host's load-hash diagnostic. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define CC_SHA256_DIGEST_LENGTH 32
typedef unsigned int CC_LONG;
typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t buf[64];
    uint32_t used;
} CC_SHA256_CTX;

int CC_SHA256_Init(CC_SHA256_CTX *ctx);
int CC_SHA256_Update(CC_SHA256_CTX *ctx, const void *data, unsigned int len);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *ctx);
unsigned char *CC_SHA256(const void *data, unsigned int len, unsigned char *md);
