#include "eccverf/EccVerf.h"

using namespace std;

EC_GROUP *eliptic = NULL;

EC_GROUP * ecies_group() {
    EC_GROUP *group;

    if (eliptic) {
        return EC_GROUP_dup(eliptic);
    }

    if (!(group = EC_GROUP_new_by_curve_name(ECIES_CURVE))) {
        printf("EC_GROUP_new_by_curve_name failed. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    else if (EC_GROUP_precompute_mult(group, NULL) != 1) {
        printf("EC_GROUP_precompute_mult failed. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
        EC_GROUP_free(group);
        return NULL;
    }

    EC_GROUP_set_point_conversion_form(group, POINT_CONVERSION_COMPRESSED);

    return EC_GROUP_dup(group);
}

EC_KEY * ecies_key_create() {
    EC_GROUP *group;
    EC_KEY *key = NULL;

    if (!(key = EC_KEY_new())) {
            printf("EC_KEY_new failed. {error = %s}\n",
                   ERR_error_string(ERR_get_error(), NULL));
            return NULL;
    }

    if (!(group = ecies_group())) {
            EC_KEY_free(key);
            return NULL;
    }

    if (EC_KEY_set_group(key, group) != 1) {
            printf("EC_KEY_set_group failed. {error = %s}\n",
                   ERR_error_string(ERR_get_error(), NULL));
            EC_GROUP_free(group);
            EC_KEY_free(key);
            return NULL;
    }

    EC_GROUP_free(group);

    if (EC_KEY_generate_key(key) != 1) {
            printf("EC_KEY_generate_key failed. {error = %s}\n",
                   ERR_error_string(ERR_get_error(), NULL));
            EC_KEY_free(key);
            return NULL;
    }

    return key;
}

char * ecies_key_public_get_hex(EC_KEY *key) {

    char *hex;
    const EC_POINT *point;
    const EC_GROUP *group;

    if (!(point = EC_KEY_get0_public_key(key))) {
        printf("EC_KEY_get0_public_key\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    if (!(group = EC_KEY_get0_group(key))) {
        printf("EC_KEY_get0_group\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    if (!(hex = EC_POINT_point2hex(group, point,
                                 POINT_CONVERSION_COMPRESSED, NULL))) {
        printf("EC_POINT_point2hex\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    //printf("PUB: %s\n", hex);
    return hex;
}

char * ecies_key_private_get_hex(EC_KEY *key) {

    char *hex;
    const BIGNUM *bn;

    if (!(bn = EC_KEY_get0_private_key(key))) {
        printf("EC_KEY_get0_private_key\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    if (!(hex = BN_bn2hex(bn))) {
        printf("BN_bn2hex\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    return hex;
}

secure_t* ecies_encrypt(char *key, unsigned char *data, size_t length) {

    void *body;
    HMAC_CTX hmac;
    int body_length;
    secure_t *cryptex;
    EVP_CIPHER_CTX cipher;
    unsigned int mac_length;
    EC_KEY *user, *ephemeral;
    size_t envelope_length, block_length, key_length;
    unsigned char envelope_key[SHA512_DIGEST_LENGTH],
            iv[EVP_MAX_IV_LENGTH], block[EVP_MAX_BLOCK_LENGTH];

    // Simple sanity check.
    if (!key || !data || !length) {
        printf("Invalid parameters passed in.\n");
        return NULL;
    }

    // Make sure we are generating enough key material for the symmetric ciphers.
    if ((key_length = EVP_CIPHER_key_length(ECIES_CIPHER)) * 2 > SHA512_DIGEST_LENGTH) {
        printf("The key derivation method will not produce enough envelope key material "
               "for the chosen ciphers. "
               "{envelope = %i / required = %zu}", SHA512_DIGEST_LENGTH / 8,
                       (key_length * 2) / 8);
        return NULL;
    }

    // Convert the user's public key from hex into a full EC_KEY structure.
    if (!(user = ecies_key_create_public_hex(key))) {
        printf("Invalid public key provided.\n");
        return NULL;
    }

    // Create the ephemeral key used specifically for this block of data.
    else if (!(ephemeral = ecies_key_create())) {
        printf("An error occurred while trying "
               "to generate the ephemeral key.\n");
        EC_KEY_free(user);
        return NULL;
    }

    // Use the intersection of the provided keys to generate the envelope
    // data used by the ciphers below. The ecies_key_derivation() function uses
    // SHA 512 to ensure we have a sufficient amount of envelope key
    // material and that the material created is sufficiently secure.
    else if (ECDH_compute_key(envelope_key, SHA512_DIGEST_LENGTH,
              EC_KEY_get0_public_key(user), ephemeral, ecies_key_derivation) !=SHA512_DIGEST_LENGTH) {
        printf("An error occurred while trying to compute the envelope key. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
        EC_KEY_free(ephemeral);
        EC_KEY_free(user);
        return NULL;
    }

    // Determine the envelope and block lengths so we can allocate a buffer for the result.
    else if ((block_length = EVP_CIPHER_block_size(ECIES_CIPHER)) == 0 ||
             block_length > EVP_MAX_BLOCK_LENGTH ||
             (envelope_length = EC_POINT_point2oct(EC_KEY_get0_group(ephemeral), EC_KEY_get0_public_key(ephemeral),
                                               POINT_CONVERSION_COMPRESSED, NULL, 0, NULL)) == 0) {
        printf("Invalid block or envelope length. {block = %zu / envelope = %zu}\n",
               block_length, envelope_length);
        EC_KEY_free(ephemeral);
        EC_KEY_free(user);
        return NULL;
    }

        // We use a conditional to pad the length if the input buffer is not evenly divisible by the block size.
        else if (!(cryptex = secure_alloc(envelope_length, EVP_MD_size(ECIES_HASHER),
                                          length, length + (length % block_length ?
                                                            (block_length - (length % block_length)) : 0)))) {
        printf("Unable to allocate a secure_t buffer to hold the encrypted result.\n");
        EC_KEY_free(ephemeral);
        EC_KEY_free(user);
        return NULL;
    }

    // Store the public key portion of the ephemeral key.
    else if (EC_POINT_point2oct(EC_KEY_get0_group(ephemeral),
                                EC_KEY_get0_public_key(ephemeral),
                                POINT_CONVERSION_COMPRESSED,
                                (unsigned char*) secure_key_data(cryptex), envelope_length, NULL) != envelope_length) {
        printf("An error occurred while trying to record the public portion of the envelope key. {error = %s}\n",
               ERR_error_string(ERR_get_error(),
                                NULL));
        EC_KEY_free(ephemeral);
        EC_KEY_free(user);
        secure_free(cryptex);
        return NULL;
    }

    // The envelope key has been stored so we no longer need to keep the keys around.
    EC_KEY_free(ephemeral);
    EC_KEY_free(user);

    // For now we use an empty initialization vector.
    memset(iv, 0, EVP_MAX_IV_LENGTH);

    // Setup the cipher context, the body length, and store a pointer to the body buffer location.
    EVP_CIPHER_CTX_init(&cipher);
    body = secure_body_data(cryptex);
    body_length = secure_body_length(cryptex);

    // Initialize the cipher with the envelope key.
    if (EVP_EncryptInit_ex(&cipher, ECIES_CIPHER, NULL, envelope_key, iv)
            != 1 || EVP_CIPHER_CTX_set_padding(&cipher, 0) != 1 ||
            EVP_EncryptUpdate(&cipher, (unsigned char *)body, &body_length, data, length - (length % block_length)) != 1) {
        printf("An error occurred while trying to secure the data using the chosen symmetric cipher. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
        EVP_CIPHER_CTX_cleanup(&cipher);
        secure_free(cryptex);
        return NULL;
    }

    // Check whether all of the data was encrypted. If they don't match up,
    // we either have a partial block remaining, or an error occurred.
    else if (body_length != length) {
        // Make sure all that remains is a partial block, and their wasn't an error.
        if (length - body_length >= block_length) {
            printf("Unable to secure the data using the chosen symmetric cipher. {error = %s}\n",
                   ERR_error_string(ERR_get_error(), NULL));
            EVP_CIPHER_CTX_cleanup(&cipher);
            secure_free(cryptex);
            return NULL;
        }

        // Copy the remaining data into our partial block buffer. The memset() call ensures any extra bytes will be zero'ed out.
        memset(block, 0, EVP_MAX_BLOCK_LENGTH);
        memcpy(block, data + body_length, length - body_length);

        // Advance the body pointer to the location of the remaining space, and calculate just how much room is still available.
        body += body_length;
        if ((body_length = secure_body_length(cryptex) - body_length) < 0) {
            printf("The symmetric cipher overflowed!\n");
            EVP_CIPHER_CTX_cleanup(&cipher);
            secure_free(cryptex);
            return NULL;
        }

        // Pass the final partially filled data block into the cipher as a complete block. The padding will be removed during the decryption process.
        else if (EVP_EncryptUpdate(&cipher, (unsigned char*) body, &body_length, block, block_length) != 1) {
            printf("Unable to secure the data using the chosen symmetric cipher. {error = %s}\n",
                   ERR_error_string(ERR_get_error(), NULL));
            EVP_CIPHER_CTX_cleanup(&cipher);
            secure_free(cryptex);
            return NULL;
        }

    }

    // Advance the pointer, then use pointer arithmetic to calculate how much of the body buffer has been used.
    // The complex logic is needed so that we get
    // the correct status regardless of whether there was a partial data block.
//    body += body_length;
//    if ((body_length = secure_body_length(cryptex) - (body - secure_body_data(cryptex))) < 0) {
//        printf("The symmetric cipher overflowed!\n");
//        EVP_CIPHER_CTX_cleanup(&cipher);
//        secure_free(cryptex);
//        return NULL;
//    }

    if (EVP_EncryptFinal_ex(&cipher, (unsigned char*) body, &body_length) != 1) {
        printf("Unable to secure the data using the chosen symmetric cipher. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
        EVP_CIPHER_CTX_cleanup(&cipher);
        secure_free(cryptex);
        return NULL;
    }

    EVP_CIPHER_CTX_cleanup(&cipher);

    // Generate an authenticated hash which can be used to validate the data during decryption.
    HMAC_CTX_init(&hmac);
    mac_length = secure_mac_length(cryptex);

    // At the moment we are generating the hash using encrypted data. At some point we may want to validate the original text instead.
    if (HMAC_Init_ex(&hmac, envelope_key + key_length, key_length, ECIES_HASHER, NULL) != 1
            || HMAC_Update(&hmac, (const unsigned char*) secure_body_data(cryptex),
                           secure_body_length(cryptex))!= 1
            || HMAC_Final(&hmac, (unsigned char*) secure_mac_data(cryptex), &mac_length) != 1) {
        printf("Unable to generate a data authentication code. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
        HMAC_CTX_cleanup(&hmac);
        secure_free(cryptex);
        return NULL;
    }

    HMAC_CTX_cleanup(&hmac);

    return cryptex;
}

unsigned char * ecies_decrypt(char *key, secure_t *cryptex, size_t *length) {

    HMAC_CTX hmac;
    size_t key_length;
    int output_length;
    EVP_CIPHER_CTX cipher;
    EC_KEY *user, *ephemeral;
    unsigned int mac_length = EVP_MAX_MD_SIZE;
    unsigned char envelope_key[SHA512_DIGEST_LENGTH], iv[EVP_MAX_IV_LENGTH], md[EVP_MAX_MD_SIZE], *block, *output;

    // Simple sanity check.
    if (!key || !cryptex || !length) {
            printf("Invalid parameters passed in.\n");
            return NULL;
    }

    // Make sure we are generating enough key material for the symmetric ciphers.
    else if ((key_length = EVP_CIPHER_key_length(ECIES_CIPHER)) * 2 > SHA512_DIGEST_LENGTH) {
                printf("The key derivation method will not produce enough envelope key material for the chosen ciphers. "
                       "{envelope = %i / required = %zu}", SHA512_DIGEST_LENGTH / 8, (key_length * 2) / 8);
                return NULL;
    }

    // Convert the user's public key from hex into a full EC_KEY structure.
    else if (!(user = ecies_key_create_private_hex(key))) {
        printf("Invalid private key provided.\n");
        return NULL;
    }

    // Create the ephemeral key used specifically for this block of data.
    else if (!(ephemeral = ecies_key_create_public_octets((unsigned char*) secure_key_data(cryptex), secure_key_length(cryptex)))) {
        printf("An error occurred while trying to recreate the ephemeral key.\n");
        EC_KEY_free(user);
        return NULL;
    }

    // Use the intersection of the provided keys to generate the envelope data used by the ciphers below. The ecies_key_derivation() function uses
    // SHA 512 to ensure we have a sufficient amount of envelope key material and that the material created is sufficiently secure.
    else if (ECDH_compute_key(envelope_key, SHA512_DIGEST_LENGTH,
                              EC_KEY_get0_public_key(ephemeral), user, ecies_key_derivation) != SHA512_DIGEST_LENGTH) {
        printf("An error occurred while trying to compute the envelope key. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
            EC_KEY_free(ephemeral);
            EC_KEY_free(user);
            return NULL;
        }

    // The envelope key material has been extracted, so we no longer need the user and ephemeral keys.
    EC_KEY_free(ephemeral);
    EC_KEY_free(user);

    // Use the authenticated hash of the ciphered data to ensure it was notmodified after being encrypted.
    HMAC_CTX_init(&hmac);

        // At the moment we are generating the hash using encrypted data. At some point we may want to validate the original text instead.
    if (HMAC_Init_ex(&hmac, envelope_key + key_length, key_length, ECIES_HASHER, NULL) != 1 ||
            HMAC_Update(&hmac, (unsigned char*) secure_body_data(cryptex), secure_body_length(cryptex))
                        != 1 || HMAC_Final(&hmac, md, &mac_length) != 1) {
        printf("Unable to generate the authentication code needed for validation. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
        HMAC_CTX_cleanup(&hmac);
        return NULL;
    }

    HMAC_CTX_cleanup(&hmac);

    // We can use the generated hash to ensure the encrypted data was not altered after being encrypted.
    if (mac_length != secure_mac_length(cryptex) || memcmp(md, secure_mac_data(cryptex), mac_length)) {
        printf("The authentication code was invalid! The ciphered data has been corrupted!\n");
        return NULL;
    }

    // Create a buffer to hold the result.
    output_length = secure_body_length(cryptex);
    if (!(block = output = (unsigned char*) malloc(output_length + 1))) {
        printf("An error occurred while trying to allocate memory for the decrypted data.\n");
        return NULL;
    }

    // For now we use an empty initialization vector. We also clear out the result buffer just to be on the safe side.
    memset(iv, 0, EVP_MAX_IV_LENGTH);
    memset(output, 0, output_length + 1);

    EVP_CIPHER_CTX_init(&cipher);

    // Decrypt the data using the chosen symmetric cipher.
    if (EVP_DecryptInit_ex(&cipher, ECIES_CIPHER, NULL, envelope_key, iv) != 1
            || EVP_CIPHER_CTX_set_padding(&cipher, 0) != 1
            || EVP_DecryptUpdate(&cipher, block,
                                 &output_length, (unsigned char*) secure_body_data(cryptex), secure_body_length(cryptex)) != 1) {
        printf("Unable to decrypt the data using the chosen symmetric cipher. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
        EVP_CIPHER_CTX_cleanup(&cipher);
        free(output);
        return NULL;
    }

    block += output_length;
    if ((output_length = secure_body_length(cryptex) - output_length) != 0) {
        printf("The symmetric cipher failed to properly decrypt the correct amount of data!\n");
        EVP_CIPHER_CTX_cleanup(&cipher);
        free(output);
        return NULL;
    }

    if (EVP_DecryptFinal_ex(&cipher, block, &output_length) != 1) {
        printf("Unable to decrypt the data using the chosen symmetric cipher. {error = %s}\n",
               ERR_error_string(ERR_get_error(), NULL));
        EVP_CIPHER_CTX_cleanup(&cipher);
        free(output);
        return NULL;
    }

    EVP_CIPHER_CTX_cleanup(&cipher);

    *length = secure_orig_length(cryptex);
    return output;
}

EC_KEY * ecies_key_create_public_hex(char *hex) {
    EC_GROUP *group;
    EC_KEY *key = NULL;
    EC_POINT *point = NULL;
    if (!(key = EC_KEY_new())) {
        printf("EC_KEY_new\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    if (!(group = EC_GROUP_new_by_curve_name(ECIES_CURVE))) {
        printf("EC_GROUP_new_by_curve_name failed. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
        EC_KEY_free(key);
        return NULL;
    }

    EC_GROUP_set_point_conversion_form(group, POINT_CONVERSION_COMPRESSED);

    if (EC_KEY_set_group(key, group) != 1) {
        printf("EC_KEY_set_group\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        EC_GROUP_free(group);
        EC_KEY_free(key);
        return NULL;
    }

    if (!(point = EC_POINT_hex2point(group, hex, NULL, NULL))) {
        printf("EC_POINT_hex2point\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        EC_KEY_free(key);
        return NULL;
    }

    if (EC_KEY_set_public_key(key, point) != 1) {
        printf("EC_KEY_set_public_key\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        EC_GROUP_free(group);
        EC_POINT_free(point);
        EC_KEY_free(key);
        return NULL;
    }

    EC_GROUP_free(group);
    EC_POINT_free(point);

    if (EC_KEY_check_key(key) != 1) {
        printf("EC_KEY_check_key\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        EC_KEY_free(key);
        return NULL;
    }

    return key;
}

EC_KEY * ecies_key_create_private_hex(char *hex) {
    EC_GROUP *group;
    BIGNUM *bn = NULL;
    EC_KEY *key = NULL;

    if (!(key = EC_KEY_new())) {
        printf("EC_KEY_new\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    if (!(group = EC_GROUP_new_by_curve_name(ECIES_CURVE))) {
        printf("EC_GROUP_new_by_curve_name failed. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
        EC_KEY_free(key);
        return NULL;
    }

    EC_GROUP_set_point_conversion_form(group, POINT_CONVERSION_COMPRESSED);

    if (EC_KEY_set_group(key, group) != 1) {
        printf("EC_KEY_set_group\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        EC_GROUP_free(group);
        EC_KEY_free(key);
        return NULL;
    }

    EC_GROUP_free(group);

    if (!(BN_hex2bn(&bn, hex))) {
        printf("BN_hex2bn\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        EC_KEY_free(key);
        return NULL;
    }

    if (EC_KEY_set_private_key(key, bn) != 1) {
        printf("EC_KEY_set_public_key\n");
        printf("%s\n", ERR_error_string(ERR_get_error(), NULL));
        EC_KEY_free(key);
        BN_free(bn);
        return NULL;
    }

    BN_free(bn);

    return key;
}

void * ecies_key_derivation(const void *input, size_t ilen, void *output, size_t *olen) {
    if (*olen < SHA512_DIGEST_LENGTH) {
            return NULL;
    }
    *olen = SHA512_DIGEST_LENGTH;
    return SHA512((unsigned char*) input, ilen, (unsigned char*) output);
}

EC_KEY * ecies_key_create_public_octets(unsigned char *octets, size_t length) {

    EC_GROUP *group;
    EC_KEY *key = NULL;
    EC_POINT *point = NULL;

    if (!(key = EC_KEY_new())) {
        printf("EC_KEY_new failed. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
                return NULL;
    }

    if (!(group = ecies_group())) {
        EC_KEY_free(key);
        return NULL;
    }

    if (EC_KEY_set_group(key, group) != 1) {
        printf("EC_KEY_set_group failed. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
        EC_GROUP_free(group);
        EC_KEY_free(key);
        return NULL;
    }

    if (!(point = EC_POINT_new(group))) {
        printf("EC_POINT_new failed. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
        EC_GROUP_free(group);
        EC_KEY_free(key);
        return NULL;
    }

    if (EC_POINT_oct2point(group, point, octets, length, NULL) != 1) {
        printf("EC_POINT_oct2point failed. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
        EC_GROUP_free(group);
        EC_KEY_free(key);
        return NULL;
    }

    if (EC_KEY_set_public_key(key, point) != 1) {
        printf("EC_KEY_set_public_key failed. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
        EC_GROUP_free(group);
        EC_POINT_free(point);
        EC_KEY_free(key);
        return NULL;
    }

    EC_GROUP_free(group);
    EC_POINT_free(point);

    if (EC_KEY_check_key(key) != 1) {
        printf("EC_KEY_check_key failed. {error = %s}\n", ERR_error_string(ERR_get_error(), NULL));
        EC_KEY_free(key);
        return NULL;
    }

    return key;
}

uint64_t secure_orig_length(secure_t *cryptex) {
    secure_head_t *head = (secure_head_t *)cryptex;
    return head->length.orig;
}

secure_t * secure_alloc(uint64_t key, uint64_t mac, uint64_t orig, uint64_t body) {
    secure_t *cryptex = (secure_t *) malloc(sizeof(secure_head_t) + key + mac + body);
    secure_head_t *head = (secure_head_t *)cryptex;
    head->length.key = key;
    head->length.mac = mac;
    head->length.orig = orig;
    head->length.body = body;
    return cryptex;
}

secure_t * secure_key_data(secure_t *cryptex) {
    return cryptex + sizeof(secure_head_t);
}

void secure_free(secure_t *cryptex) {
    free(cryptex);
    return;
}


uint64_t secure_body_length(secure_t *cryptex) {
    secure_head_t *head = (secure_head_t *)cryptex;
    return head->length.body;
}

secure_t * secure_body_data(secure_t *cryptex) {
    secure_head_t *head = (secure_head_t *)cryptex;
    return cryptex + (sizeof(secure_head_t) + head->length.key + head->length.mac);
}

secure_t * secure_mac_data(secure_t *cryptex) {
    secure_head_t *head = (secure_head_t *)cryptex;
    return cryptex + (sizeof(secure_head_t) + head->length.key);
}


uint64_t secure_mac_length(secure_t *cryptex) {
    secure_head_t *head = (secure_head_t *)cryptex;
    return head->length.mac;
}

uint64_t secure_key_length(secure_t *cryptex) {
    secure_head_t *head = (secure_head_t *)cryptex;
    return head->length.key;
}

void test() {
    EC_KEY *key;
    key = ecies_key_create();
    if (key == NULL) {
        cout << "Key generation failed!" << endl;
    }
    else {
        char* hex_pub = ecies_key_public_get_hex(key);

        int tlen;
        char const *text = "This is a test";

        cout << text << endl;

//        do {
//            tlen = (rand() % (1024 * 1024));
//        } while (tlen < 1024);
//        if (!(text = (unsigned char *) malloc(tlen + 1)) || !(copy = (unsigned char *) malloc(tlen + 1))) {
//            printf("Memory error.\n");

//        }

//        // Wipe and then fill the data blocks with random data.
//        memset(copy, 0, tlen + 1);
//        memset(text, 0, tlen + 1);
//        for (uint64_t j = 0; j < tlen; j++) {
//                *(copy + j) = *(text + j) = (rand() % 255);
//        }

//        secure_t *ciphered = NULL;
//        if (!(ciphered = ecies_encrypt(hex_pub, text, tlen))) {
//            printf("The encryption process failed!\n");
//            cout << "error" << endl;
//        }

        secure_t *ciphered = NULL;
        ciphered = ecies_encrypt(hex_pub, (unsigned char*) text, sizeof(text)*sizeof(char));
        cout << (char*) ciphered << endl;

    }
}

