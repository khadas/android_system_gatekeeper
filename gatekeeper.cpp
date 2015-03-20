/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <time.h>
#include <iostream>
#include <iomanip>
#include <UniquePtr.h>

#include <gatekeeper/gatekeeper.h>

namespace gatekeeper {

/**
 * Internal only structure for easy serialization
 * and deserialization of password handles.
 */
static const uint8_t HANDLE_VERSION = 0;
struct __attribute__ ((__packed__)) password_handle_t {
    // fields included in signature
    uint8_t version = HANDLE_VERSION;
    secure_id_t user_id;
    secure_id_t authenticator_id;

    // fields not included in signature
    salt_t salt;
    uint8_t signature[32];
};

void GateKeeper::Enroll(const EnrollRequest &request, EnrollResponse *response) {
    if (response == NULL) return;

    if (!request.provided_password.buffer.get()) {
        response->error = ERROR_INVALID;
        return;
    }

    secure_id_t user_id = 0;
    uint8_t *current_password = NULL;
    size_t current_password_size = 0;

    if (request.password_handle.buffer.get() == NULL) {
        // Password handle does not match what is stored, generate new SecureID
        GetRandom(&user_id, sizeof(secure_id_t));
    } else {
        if (!ValidatePasswordFile(request.user_id, request.password_handle)) {
           response->error = ERROR_INVALID;
           return;
        } else {
            // Password handle matches password file
            password_handle_t *pw_handle =
                reinterpret_cast<password_handle_t *>(request.password_handle.buffer.get());
            if (!DoVerify(pw_handle, request.enrolled_password)) {
                // incorrect old password
                response->error = ERROR_INVALID;
                return;
            }

            user_id = pw_handle->user_id;
        }
    }

    salt_t salt;
    GetRandom(&salt, sizeof(salt));

    secure_id_t authenticator_id;
    GetRandom(&authenticator_id, sizeof(authenticator_id));


    SizedBuffer password_handle;
    if(!CreatePasswordHandle(&password_handle,
            salt, user_id, authenticator_id, request.provided_password.buffer.get(),
            request.provided_password.length)) {
        response->error = ERROR_INVALID;
        return;
    }


    WritePasswordFile(request.user_id, password_handle);

    response->SetEnrolledPasswordHandle(&password_handle);
}

void GateKeeper::Verify(const VerifyRequest &request, VerifyResponse *response) {
    if (response == NULL) return;

    if (!request.provided_password.buffer.get() || !request.password_handle.buffer.get()) {
        response->error = ERROR_INVALID;
        return;
    }

    secure_id_t user_id, authenticator_id;
    password_handle_t *password_handle = reinterpret_cast<password_handle_t *>(
            request.password_handle.buffer.get());

    // Sanity check
    if (password_handle->version != HANDLE_VERSION) {
        response->error = ERROR_INVALID;
        return;
    }

    if (!ValidatePasswordFile(request.user_id, request.password_handle)) {
        // we don't allow access to keys if we can't validate the file.
        // we must allow this case to support authentication before we decrypt
        // /data, however.
        user_id = 0;
        authenticator_id = 0;
    } else {
        user_id = password_handle->user_id;
        authenticator_id = password_handle->authenticator_id;
    }

    struct timespec time;
    uint64_t timestamp;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);
    timestamp = static_cast<uint32_t>(time.tv_sec);

    if (DoVerify(password_handle, request.provided_password)) {
        // Signature matches
        SizedBuffer auth_token;
        MintAuthToken(&auth_token.buffer, &auth_token.length, timestamp,
                user_id, authenticator_id);
        response->SetVerificationToken(&auth_token);
    } else {
        response->error = ERROR_INVALID;
    }
}

bool GateKeeper::CreatePasswordHandle(SizedBuffer *password_handle_buffer, salt_t salt,
        secure_id_t user_id, secure_id_t authenticator_id, const uint8_t *password,
        size_t password_length) {
    password_handle_buffer->buffer.reset(new uint8_t[sizeof(password_handle_t)]);
    password_handle_buffer->length = sizeof(password_handle_t);

    password_handle_t *password_handle = reinterpret_cast<password_handle_t *>(
            password_handle_buffer->buffer.get());
    password_handle->version = HANDLE_VERSION;
    password_handle->salt = salt;
    password_handle->user_id = user_id;
    password_handle->authenticator_id = authenticator_id;

    size_t metadata_length = sizeof(user_id) /* user id */
        + sizeof(authenticator_id) /* auth id */ + sizeof(uint8_t) /* version */;
    uint8_t to_sign[password_length + metadata_length];
    memcpy(to_sign, &password_handle->version, metadata_length);
    memcpy(to_sign + metadata_length, password, password_length);

    UniquePtr<uint8_t> password_key;
    size_t password_key_length = 0;
    GetPasswordKey(&password_key, &password_key_length);

    if (!password_key.get() || password_key_length == 0) {
        return false;
    }

    ComputePasswordSignature(password_handle->signature, sizeof(password_handle->signature),
            password_key.get(), password_key_length, to_sign, sizeof(to_sign), salt);
    return true;
}

bool GateKeeper::DoVerify(const password_handle_t *expected_handle, const SizedBuffer &password) {
    if (!password.buffer.get()) return false;

    SizedBuffer provided_handle;
    if (!CreatePasswordHandle(&provided_handle, expected_handle->salt, expected_handle->user_id,
            expected_handle->authenticator_id, password.buffer.get(), password.length)) {
        return false;
    }

    return memcmp_s(provided_handle.buffer.get(), expected_handle, sizeof(*expected_handle)) == 0;
}

bool GateKeeper::ValidatePasswordFile(uint32_t uid, const SizedBuffer &provided_handle) {
    SizedBuffer stored_handle;
    ReadPasswordFile(uid, &stored_handle);

    if (!stored_handle.buffer.get() || stored_handle.length == 0) return false;

    // do we also verify the signature here?
    return stored_handle.length == provided_handle.length &&
        memcmp_s(stored_handle.buffer.get(), provided_handle.buffer.get(), stored_handle.length)
            == 0;
}

void GateKeeper::MintAuthToken(UniquePtr<uint8_t> *auth_token, size_t *length,
        uint32_t timestamp, secure_id_t user_id, secure_id_t authenticator_id) {
    if (auth_token == NULL) return;

    AuthToken *token = new AuthToken;
    SizedBuffer serialized_auth_token;

    token->root_secure_user_id = user_id;
    token->auxiliary_secure_user_id = authenticator_id;
    token->timestamp = timestamp;

    UniquePtr<uint8_t> auth_token_key;
    size_t key_len;
    GetAuthTokenKey(&auth_token_key, &key_len);

    size_t hash_len = (size_t)((uint8_t *)&token->hmac - (uint8_t *)token);
    ComputeSignature(token->hmac, sizeof(token->hmac), auth_token_key.get(), key_len,
            reinterpret_cast<uint8_t *>(token), hash_len);

    if (length != NULL) *length = sizeof(AuthToken);
    auth_token->reset(reinterpret_cast<uint8_t *>(token));
}

}