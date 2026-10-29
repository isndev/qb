/*
 * A decoy (Huly QB-1180): qb-io-test-unit-openssl-headers links this directory as a SYSTEM include of its own,
 * after qb-io, the way a prefix-wide include such as /opt/homebrew/include comes in. Reaching this file means it was
 * searched before the OpenSSL the configure found.
 */
#error "qb-io-test-unit-openssl-headers: a prefix-wide include directory came before the found OpenSSL (Huly QB-1180)"
