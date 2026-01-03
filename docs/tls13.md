# TLS 1.3 plan for `https://de.wikipedia.org` (minimal, tool-driven)

Goal: build a minimal, testable TLS 1.3 client stack for this OS that can fetch content from `de.wikipedia.org` over HTTPS.

Constraints/knowns:
- Server certificate chain uses **ECDSA** with an **EC 256-bit key** (P-256) and certificate signature **`sha384WithECDSA`** (common).
- Negotiated TLS 1.3 cipher suite: **`TLS_AES_128_GCM_SHA256` (0x1301)**.
  - Important: in TLS 1.3 the cipher suite determines **HKDF hash** (here **SHA-256**) and **AEAD** (here **AES-128-GCM**).
  - The certificate’s signature algorithm (here **ECDSA+SHA-384**) is separate.

Minimal MVP note:
- For bringup we can **skip certificate validation** and accept any server certificate.
- This still provides encryption on the wire, but it is **unauthenticated** and therefore vulnerable to active MITM.
- Even in this MVP, we still need the TLS 1.3 transcript hash + Finished verification to interoperate with real servers.

Approach: implement a series of small standalone tools (each runnable in isolation, with hard-coded test vectors) and then compose them into a TLS 1.3 client + an HTTP/1.1 GET.

---

## 0) Prerequisites (already mostly present)

- **IPv6 + routing**: working (RA/SLAAC/RDNSS, `ping6` works).
- **DNS**: `dns6` is available; ensure it is reliable.
- **TCP connect**: confirm we have an IPv6 TCP client that can connect to `de.wikipedia.org:443` (or implement one if missing).

Standalone tool(s):
- `tcp6_connect`: connect to `[addr]:443`, read/write raw bytes.
- `dns6` improvements: ensure timeouts work reliably for UDP receive.

Acceptance:
- Can establish a TCP connection and exchange bytes with a simple server (or at least connect successfully).

---

## 1) Byte/encoding foundation tools

### 1.1 `bytes`: safe helpers
Purpose: avoid subtle parsing bugs.
- Big-endian load/store helpers (`be16`, `be24`, `be32`, `be64`)
- Bounded cursor reader/writer for packet construction
- Constant-time compare (for MAC/tag checks)

Acceptance:
- Unit tests for cursor bounds.

### 1.2 `asn1_der`: minimal DER decoder (optional for MVP)
Purpose: parse X.509 certs (DER/ASN.1) for pinning/validation later.

MVP:
- Not required if we accept any certificate and treat Certificate/CertificateVerify as opaque handshake bytes.

Minimum DER support (when enabled):
- SEQUENCE, SET
- INTEGER, BIT STRING, OCTET STRING
- OBJECT IDENTIFIER
- UTCTime/GeneralizedTime
- Context-specific constructed tags `[0]`, `[1]`, `[2]` used in X.509

Output model:
- A streaming/bounded decoder that can “skip unknown” elements.

Acceptance:
- Parse a known DER test file and extract a few fields.

---

## 2) Cryptographic primitives (standalone + tested)

You already have useful reference code in `archive/monacc-x86/core/` (AES/GCM/HKDF/HMAC/SHA256). The plan is to port the algorithms (not necessarily the whole library) to AArch64 kernel/userland with the same API shape.

### 2.1 Hashes
Tools:
- `sha256` (required by cipher suite 0x1301)
- `sha384` (optional; required only if we later verify `sha384WithECDSA` certificate signatures)

Acceptance:
- NIST test vectors for SHA-256 and SHA-384.

### 2.2 HMAC + HKDF
Tools:
- `hmac_sha256`
- `hkdf_extract_sha256`, `hkdf_expand_sha256`

TLS 1.3 uses HKDF-Expand-Label (a specific label format):
- `hkdf_expand_label_sha256(secret, "tls13 "+label, context, out_len)`

Acceptance:
- RFC 5869 HKDF test vectors (SHA-256).
- TLS 1.3 key schedule intermediate checks using known traces (see §7).

### 2.3 AES-128 + AES-128-GCM
Tools:
- `aes128_encrypt_block`
- `gcm_init`, `gcm_aad_update`, `gcm_crypt`, `gcm_finish_tag`

TLS 1.3 uses AEAD with:
- key length 16
- nonce length 12 (constructed from static IV XOR sequence number)
- tag length 16

Acceptance:
- NIST GCM test vectors.

### 2.4 ECC: secp256r1 (P-256) + ECDSA verify (optional for MVP)
Needed for certificate verification / pinning.
Tools:
- `p256`: field arithmetic mod p, point add/double, scalar multiply
- `ecdsa_p256_verify_sha384(pubkey, msg_hash384, sig_r, sig_s)`

Notes:
- Start with **verify only**. Signing is not required.
- You will need DER decoding for ECDSA signature format in X.509:
  - `ECDSA-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER }`

Acceptance:
- ECDSA P-256 verify test vectors.

### 2.5 ECDHE key exchange (choose minimal)
TLS 1.3 requires an ephemeral key exchange (key share extension).
Practical minimal choices:
- **X25519** (common, simpler/faster)
- **secp256r1 ECDHE** (matches “EC 256-bit” world; also common)

Plan:
- Implement one first. If unsure what QEMU host/your environment negotiates, implement **X25519** first (usually easiest) and add P-256 ECDHE as fallback.

Tools:
- `x25519` (preferred initial) OR `p256_ecdh`

Acceptance:
- RFC 7748 X25519 test vectors or P-256 ECDH test vectors.

### 2.6 Randomness
TLS needs fresh random bytes:
- ClientHello random (32 bytes)
- ephemeral key shares

Tool:
- `rng_fill(buf,n)`

Minimum viable options (in order):
1. If running under QEMU: a simple entropy feed from host (semihosting) or a virt RNG if available.
2. Otherwise: timer jitter + device noise (USB timings) + hash mixing (not strong, but better than deterministic).

Acceptance:
- Never repeats across boots in QEMU; basic health checks.

---

## 3) Certificate handling (skipped for MVP)

MVP goal: complete TLS 1.3 handshakes and fetch HTTPS content while **accepting any certificate**.

What we still must do in MVP:
- Parse TLS handshake framing so we can hash the **exact** handshake message bytes into the transcript.
- Receive and process the `Certificate` and `CertificateVerify` handshake messages as opaque blobs (do not interpret their contents).

What we explicitly do not do in MVP:
- Name validation (SAN/CN)
- Chain building
- Signature verification
- Time validity checks

Later hardening options:

### 3.1 `x509_parse` (optional hardening)
Extract:
- Subject CN + SAN DNS names (match `de.wikipedia.org`)
- SPKI public key
- Signature algorithm

### 3.2 `x509_verify_chain_min` or pinning (optional hardening)
Two pragmatic models:
- **SPKI pinning** (fastest to ship securely)
- **Small CA store** (more browser-like)

---

## 4) TLS 1.3 core protocol tools

### 4.1 `tls13_record`
Implement record layer framing and AEAD protection.

Record content types:
- 22 handshake
- 21 alert
- 23 application_data

In TLS 1.3, most content is sent as `application_data` with an inner content type.

Tool API shape:
- `tls13_write_handshake(plaintext_handshake_bytes)`
- `tls13_read_record()` → returns decrypted plaintext + inner type

Acceptance:
- Can send/receive unencrypted ClientHello / ServerHello records.
- Can encrypt/decrypt records after keys are derived.

### 4.2 `tls13_handshake_messages`
Need to serialize/parse at least:
- ClientHello
- ServerHello
- EncryptedExtensions
- Certificate
- CertificateVerify
- Finished

Extensions required for real servers:
- **SNI** (server_name = `de.wikipedia.org`)
- **supported_versions** (TLS 1.3)
- **supported_groups** (x25519, secp256r1)
- **signature_algorithms** (include ecdsa_secp256r1_sha384 at least; also include rsa_pss if you want flexibility)
- **key_share** (x25519 or p256)
- **ALPN** (offer `http/1.1` to keep it simple)

Acceptance:
- Parser can skip unknown extensions.

### 4.3 `tls13_key_schedule` (HKDF-SHA256)
For `TLS_AES_128_GCM_SHA256` implement:
- transcript hash H = SHA-256 over handshake messages
- early_secret = HKDF-Extract(0, 0)
- derived secrets per RFC 8446
- handshake traffic keys/IVs
- application traffic keys/IVs
- Finished key and verify_data

Acceptance:
- Match known-good computed values for a captured handshake transcript (see §7).

### 4.4 Finished verification (required) + CertificateVerify verification (optional)

Required (even for MVP):
- Verify server Finished using HMAC-SHA256 over the transcript hash.

Optional hardening (later):
- Verify server CertificateVerify signature using the leaf public key.
- Validate the certificate (pinning or CA chain).

Acceptance:
- Handshake completes and application keys installed.

---

## 5) HTTP/1.1 tool

### 5.1 `http1_client`
Minimal request:
- `GET /wiki/Hauptseite HTTP/1.1\r\n`
- `Host: de.wikipedia.org\r\n`
- `Connection: close\r\n`
- `User-Agent: mona-rpzero/0\r\n`
- `Accept: text/html\r\n`
- `\r\n`

Acceptance:
- Parses status line + headers; prints first N bytes of body.

---

## 6) Integration order (recommended)

MVP (no cert validation):
1. Crypto: `sha256`, `hkdf_sha256`, `aes128_gcm` (reuse/port from `archive/monacc-x86/core/` where helpful)
2. Record framing (plaintext only), ClientHello serialization
3. ECDHE key share (X25519 recommended first)
4. Handshake keys + encrypt/decrypt handshake records
5. Transcript hash + Finished verification
6. Application data keys + HTTP GET

Hardening (later):
7. `asn1_der` + `x509_parse`
8. `sha384` + `p256` + `ecdsa_p256_verify_sha384` + pinning/chain validation

---

## 7) How to test each tool (without guessing)

You want deterministic, automatable tests.

### 7.1 Use known vectors
- SHA: NIST
- HKDF: RFC 5869
- X25519: RFC 7748
- AES-GCM: NIST
- ECDSA P-256 verify: standard vectors (only needed once cert verification is enabled)

### 7.2 Capture one real handshake and replay
On the host, capture a TLS 1.3 handshake to `de.wikipedia.org` and keep:
- ClientHello bytes
- ServerHello..Finished bytes
- The negotiated group/signature algorithm

Then:
- Feed the transcript into your key schedule tool and compare derived secrets/keys against a reference implementation (host-side script).

Practical workflow:
- Add a host-side helper script (Python/OpenSSL) that prints:
  - transcript hash values at each step
  - handshake traffic secrets
  - Finished verify_data

This avoids "it should work" debugging.

---

## 8) Minimal security posture (what we will and won’t do initially)

Initial MVP (bringup):
- **No certificate validation** (accept any certificate, do not check names or chain).
- Verify server Finished (required for interop).

Implication:
- This is vulnerable to active MITM. It is useful for bootstrapping the protocol stack, not for security.

Fast hardening steps once MVP works:
- **TOFU pinning**: remember the first-seen SPKI hash for `de.wikipedia.org` and require it on subsequent connects.
- Or ship a fixed SPKI pin (most deterministic).

Defer:
- OCSP/CRL
- Full policy constraints
- Time validation if system clock is unreliable

---

## 9) Notes specific to this repo

- Crypto references exist in `archive/monacc-x86/core/`:
  - `mc_aes.c/.h`, `mc_gcm.c/.h`, `mc_hkdf.c/.h`, `mc_hmac.c/.h`, `mc_sha256.c/.h`
  These can guide algorithm porting.

- Logging: keep TLS tools quiet by default; add optional debug flags (mirroring the existing `*_DEBUG` pattern).

---

## 10) Deliverables checklist

A “minimal suite” to reach Wikipedia should include:

MVP (no cert validation):
1. `sha256`
2. `hmac_sha256`, `hkdf_sha256`, `hkdf_expand_label_sha256`
3. `aes128_gcm`
4. `x25519` (or `p256_ecdh`)
5. `tls13_record` + `tls13_handshake` + `tls13_key_schedule` + transcript hashing
6. `http1_client` over TLS

Hardening add-ons (later):
7. `asn1_der` + `x509_parse`
8. `sha384` + `p256` + `ecdsa_p256_verify_sha384` + pinning/chain validation

When all are working, the end-to-end smoke test is:
- Resolve `de.wikipedia.org` (AAAA)
- TCP connect to port 443
- TLS 1.3 handshake completes
- HTTPS GET returns `HTTP/1.1 200` and HTML body bytes
