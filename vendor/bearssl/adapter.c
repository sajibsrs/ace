#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include "bearssl.h"

/* BearSSL is deliberately contained behind fixed-width and byte-pointer
 * operations. One context owns one decoded trust anchor and hostname copy.
 */
#define ACE_DN_MAX 2048
#define ACE_KEY_MAX BR_X509_BUFSIZE_KEY
#define ACE_HOST_MAX 253

typedef struct {
    const br_x509_class *vtable;
    br_x509_decoder_context leaf;
    CFMutableArrayRef certificates;
    CFMutableDataRef current;
    char host[ACE_HOST_MAX + 1];
    size_t certificate_index;
    int failed;
} AceSystemTrust;

static void system_release_chain(AceSystemTrust *ctx) {
    if (ctx->current != NULL) CFRelease(ctx->current);
    if (ctx->certificates != NULL) CFRelease(ctx->certificates);
    ctx->current = NULL;
    ctx->certificates = NULL;
}

static void system_start_chain(const br_x509_class **opaque,
        const char *server_name) {
    AceSystemTrust *ctx = (AceSystemTrust *)opaque;
    system_release_chain(ctx);
    ctx->failed = 0;
    ctx->certificate_index = 0;
    if (server_name == NULL || server_name[0] == 0 ||
            strlen(server_name) > ACE_HOST_MAX) {
        ctx->failed = 1;
        return;
    }
    memcpy(ctx->host, server_name, strlen(server_name) + 1);
    ctx->certificates = CFArrayCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeArrayCallBacks);
    if (ctx->certificates == NULL) ctx->failed = 1;
}

static void system_start_cert(const br_x509_class **opaque, uint32_t length) {
    AceSystemTrust *ctx = (AceSystemTrust *)opaque;
    if (ctx->failed) return;
    ctx->current = CFDataCreateMutable(kCFAllocatorDefault, (CFIndex)length);
    if (ctx->current == NULL) {
        ctx->failed = 1;
        return;
    }
    if (ctx->certificate_index == 0)
        br_x509_decoder_init(&ctx->leaf, NULL, NULL);
}

static void system_append(const br_x509_class **opaque,
        const unsigned char *data, size_t len) {
    AceSystemTrust *ctx = (AceSystemTrust *)opaque;
    if (ctx->failed || ctx->current == NULL) return;
    CFDataAppendBytes(ctx->current, data, (CFIndex)len);
    if (ctx->certificate_index == 0)
        br_x509_decoder_push(&ctx->leaf, data, len);
}

static void system_end_cert(const br_x509_class **opaque) {
    AceSystemTrust *ctx = (AceSystemTrust *)opaque;
    SecCertificateRef certificate;
    if (ctx->failed || ctx->current == NULL) return;
    certificate = SecCertificateCreateWithData(kCFAllocatorDefault,
        ctx->current);
    CFRelease(ctx->current);
    ctx->current = NULL;
    if (certificate == NULL) {
        ctx->failed = 1;
        return;
    }
    CFArrayAppendValue(ctx->certificates, certificate);
    CFRelease(certificate);
    ctx->certificate_index++;
}

static unsigned system_end_chain(const br_x509_class **opaque) {
    AceSystemTrust *ctx = (AceSystemTrust *)opaque;
    CFStringRef hostname = NULL;
    SecPolicyRef policy = NULL;
    SecTrustRef trust = NULL;
    Boolean trusted = false;
    if (!ctx->failed && ctx->certificate_index != 0 &&
            br_x509_decoder_get_pkey(&ctx->leaf) != NULL) {
        hostname = CFStringCreateWithCString(kCFAllocatorDefault, ctx->host,
            kCFStringEncodingUTF8);
        if (hostname != NULL) policy = SecPolicyCreateSSL(true, hostname);
        if (!ctx->failed && policy != NULL &&
                SecTrustCreateWithCertificates(ctx->certificates,
                policy, &trust) == errSecSuccess)
            trusted = SecTrustEvaluateWithError(trust, NULL);
    }
    if (trust != NULL) CFRelease(trust);
    if (policy != NULL) CFRelease(policy);
    if (hostname != NULL) CFRelease(hostname);
    system_release_chain(ctx);
    return trusted ? 0 : BR_ERR_X509_NOT_TRUSTED;
}

static const br_x509_pkey *system_get_pkey(
        const br_x509_class *const *opaque, unsigned *usages) {
    AceSystemTrust *ctx = (AceSystemTrust *)opaque;
    br_x509_pkey *key = br_x509_decoder_get_pkey(&ctx->leaf);
    if (key != NULL && usages != NULL)
        *usages = BR_KEYTYPE_SIGN | BR_KEYTYPE_KEYX;
    return key;
}

static const br_x509_class SYSTEM_TRUST_VTABLE = {
    sizeof(AceSystemTrust),
    system_start_chain,
    system_start_cert,
    system_append,
    system_end_cert,
    system_end_chain,
    system_get_pkey,
};

typedef struct {
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    AceSystemTrust system;
    br_x509_trust_anchor anchor;
    unsigned char dn[ACE_DN_MAX];
    unsigned char key[ACE_KEY_MAX];
    size_t dn_len;
    int dn_failed;
    char host[ACE_HOST_MAX + 1];
} AceBearSSLClient;

static void append_dn(void *opaque, const void *data, size_t len) {
    AceBearSSLClient *ctx = opaque;
    if (len > ACE_DN_MAX - ctx->dn_len) {
        ctx->dn_failed = 1;
        return;
    }
    memcpy(ctx->dn + ctx->dn_len, data, len);
    ctx->dn_len += len;
}

size_t ace_tls_client_size(void) { return sizeof(AceBearSSLClient); }
size_t ace_tls_client_align(void) { return _Alignof(AceBearSSLClient); }
size_t ace_tls_record_buffer_size(void) { return BR_SSL_BUFSIZE_MONO; }

int32_t ace_tls_client_init(void *storage, void *record_buffer,
        size_t record_buffer_len, const void *entropy, size_t entropy_len,
        const void *anchor_der, size_t anchor_der_len) {
    AceBearSSLClient *ctx = storage;
    br_x509_decoder_context dc;
    br_x509_pkey *pk;
    size_t key_len;

    if (ctx == NULL || record_buffer == NULL ||
            record_buffer_len < BR_SSL_BUFSIZE_MONO || entropy == NULL ||
            entropy_len < 32 || (anchor_der == NULL && anchor_der_len != 0)) {
        return 0;
    }
    memset(ctx, 0, sizeof *ctx);
    if (anchor_der_len == 0) {
        br_ssl_client_init_full(&ctx->client, &ctx->x509, NULL, 0);
        ctx->system.vtable = &SYSTEM_TRUST_VTABLE;
        br_ssl_engine_set_x509(&ctx->client.eng, &ctx->system.vtable);
    } else {
        br_x509_decoder_init(&dc, append_dn, ctx);
        br_x509_decoder_push(&dc, anchor_der, anchor_der_len);
        pk = br_x509_decoder_get_pkey(&dc);
        if (pk == NULL || ctx->dn_failed || ctx->dn_len == 0) return 0;

        ctx->anchor.dn.data = ctx->dn;
        ctx->anchor.dn.len = ctx->dn_len;
        ctx->anchor.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
        ctx->anchor.pkey.key_type = pk->key_type;
        if (pk->key_type == BR_KEYTYPE_RSA) {
            key_len = pk->key.rsa.nlen + pk->key.rsa.elen;
            if (key_len > sizeof ctx->key) return 0;
            memcpy(ctx->key, pk->key.rsa.n, pk->key.rsa.nlen);
            memcpy(ctx->key + pk->key.rsa.nlen, pk->key.rsa.e,
                pk->key.rsa.elen);
            ctx->anchor.pkey.key.rsa.n = ctx->key;
            ctx->anchor.pkey.key.rsa.nlen = pk->key.rsa.nlen;
            ctx->anchor.pkey.key.rsa.e = ctx->key + pk->key.rsa.nlen;
            ctx->anchor.pkey.key.rsa.elen = pk->key.rsa.elen;
        } else if (pk->key_type == BR_KEYTYPE_EC) {
            key_len = pk->key.ec.qlen;
            if (key_len > sizeof ctx->key) return 0;
            memcpy(ctx->key, pk->key.ec.q, key_len);
            ctx->anchor.pkey.key.ec.curve = pk->key.ec.curve;
            ctx->anchor.pkey.key.ec.q = ctx->key;
            ctx->anchor.pkey.key.ec.qlen = key_len;
        } else {
            return 0;
        }
        br_ssl_client_init_full(&ctx->client, &ctx->x509, &ctx->anchor, 1);
    }
    br_ssl_engine_set_versions(&ctx->client.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_buffer(&ctx->client.eng, record_buffer,
        record_buffer_len, 0);
    br_ssl_engine_inject_entropy(&ctx->client.eng, entropy, entropy_len);
    ctx->client.eng.rng_os_rand_done = 1;
    return 1;
}

int32_t ace_tls_client_reset(void *storage, const void *hostname,
        size_t hostname_len) {
    AceBearSSLClient *ctx = storage;
    if (ctx == NULL || hostname == NULL || hostname_len == 0 ||
            hostname_len > ACE_HOST_MAX || memchr(hostname, 0, hostname_len)) {
        return 0;
    }
    memcpy(ctx->host, hostname, hostname_len);
    ctx->host[hostname_len] = 0;
    return br_ssl_client_reset(&ctx->client, ctx->host, 0);
}

void ace_tls_client_cleanup(void *storage) {
    AceBearSSLClient *ctx = storage;
    if (ctx != NULL && ctx->system.vtable == &SYSTEM_TRUST_VTABLE)
        system_release_chain(&ctx->system);
}

uint32_t ace_tls_client_state(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    return br_ssl_engine_current_state(&ctx->client.eng);
}

int32_t ace_tls_client_error(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    return br_ssl_engine_last_error(&ctx->client.eng);
}

size_t ace_tls_sendrec_len(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    size_t len;
    (void)br_ssl_engine_sendrec_buf(&ctx->client.eng, &len);
    return len;
}

const void *ace_tls_sendrec_ptr(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    size_t len;
    return br_ssl_engine_sendrec_buf(&ctx->client.eng, &len);
}

void ace_tls_sendrec_ack(void *storage, size_t len) {
    AceBearSSLClient *ctx = storage;
    br_ssl_engine_sendrec_ack(&ctx->client.eng, len);
}

size_t ace_tls_recvrec_len(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    size_t len;
    (void)br_ssl_engine_recvrec_buf(&ctx->client.eng, &len);
    return len;
}

void *ace_tls_recvrec_ptr(const void *storage) {
    const AceBearSSLClient *ctx = storage;
    size_t len;
    return br_ssl_engine_recvrec_buf(&ctx->client.eng, &len);
}

void ace_tls_recvrec_ack(void *storage, size_t len) {
    AceBearSSLClient *ctx = storage;
    br_ssl_engine_recvrec_ack(&ctx->client.eng, len);
}

size_t ace_tls_sendapp(void *storage, const void *data, size_t len) {
    AceBearSSLClient *ctx = storage;
    size_t cap;
    unsigned char *dst = br_ssl_engine_sendapp_buf(&ctx->client.eng, &cap);
    if (cap > len) cap = len;
    if (cap != 0) {
        memcpy(dst, data, cap);
        br_ssl_engine_sendapp_ack(&ctx->client.eng, cap);
        br_ssl_engine_flush(&ctx->client.eng, 0);
    }
    return cap;
}

size_t ace_tls_recvapp(void *storage, void *data, size_t len) {
    AceBearSSLClient *ctx = storage;
    size_t cap;
    unsigned char *src = br_ssl_engine_recvapp_buf(&ctx->client.eng, &cap);
    if (cap > len) cap = len;
    if (cap != 0) {
        memcpy(data, src, cap);
        br_ssl_engine_recvapp_ack(&ctx->client.eng, cap);
    }
    return cap;
}
