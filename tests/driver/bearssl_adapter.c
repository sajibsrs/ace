#include <stddef.h>
#include <stdint.h>

#include "bearssl.h"

/* BearSSL stays wholly behind this private proof boundary. */
typedef struct {
    br_ssl_client_context client;
    br_x509_minimal_context x509;
} AceBearSSLClient;

_Static_assert(BR_TLS12 == 0x0303, "unexpected BearSSL TLS 1.2 value");
_Static_assert(_Alignof(AceBearSSLClient) <= 16,
               "BearSSL context exceeds Ace's supported allocation alignment");

size_t ace_bearssl_client_size(void) { return sizeof(AceBearSSLClient); }
size_t ace_bearssl_client_align(void) { return _Alignof(AceBearSSLClient); }
size_t ace_bearssl_record_buffer_size(void) { return BR_SSL_BUFSIZE_MONO; }
uint32_t ace_bearssl_version_min(void) { return BR_TLS12; }
uint32_t ace_bearssl_version_max(void) { return BR_TLS12; }

int32_t ace_bearssl_client_init(void *storage, void *record_buffer,
                                size_t record_buffer_len,
                                const void *entropy, size_t entropy_len) {
    AceBearSSLClient *ctx;

    if (storage == NULL || record_buffer == NULL ||
        record_buffer_len < BR_SSL_BUFSIZE_MONO || entropy == NULL ||
        entropy_len < 32) {
        return 0;
    }
    ctx = storage;
    br_ssl_client_init_full(&ctx->client, &ctx->x509, NULL, 0);
    br_ssl_engine_set_versions(&ctx->client.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_buffer(&ctx->client.eng, record_buffer,
                             record_buffer_len, 0);
    /* Entropy acquisition is deliberately outside BearSSL. Injection marks
       the DRBG seeded; this flag suppresses BearSSL's later implicit call to
       br_prng_seeder_system() during handshake initialization. */
    br_ssl_engine_inject_entropy(&ctx->client.eng, entropy, entropy_len);
    ctx->client.eng.rng_os_rand_done = 1;
    return 1;
}

int32_t ace_bearssl_system_rng_disabled(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    return ctx != NULL && ctx->client.eng.rng_os_rand_done == 1;
}

int32_t ace_bearssl_client_random(void *storage, void *output,
                                  size_t output_len) {
    AceBearSSLClient *ctx;

    if (storage == NULL || (output == NULL && output_len != 0)) {
        return 0;
    }
    ctx = storage;
    br_hmac_drbg_generate(&ctx->client.eng.rng, output, output_len);
    return 1;
}

uint32_t ace_bearssl_client_state(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    return br_ssl_engine_current_state(&ctx->client.eng);
}

int32_t ace_bearssl_client_error(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    return br_ssl_engine_last_error(&ctx->client.eng);
}
