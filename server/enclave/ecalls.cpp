// Include the trusted modelaggregator header that is generated
// during the build. This file is generated by calling the
// sdk tool oeedger8r against the modelaggregator.edl file.
#include "modelaggregator_t.h"

#include <stdio.h>
#include <vector>
#include <numeric>
#include <map>
#include <set>
#include <string>

// Include encryption/decryption and serialization/deserialization headers
#include "encryption/encrypt.h"
#include "encryption/serialization.h"
#include "utils.h"

// Include necessary header for SIMD instructions
#include "intrinsics/immintrin.h"

using namespace std;

#define check_host_buffer(ptr, size) {                    \
if (!oe_is_outside_enclave((ptr), size)) {                \
    fprintf(stderr,                                       \
            "%s:%d: Buffer bounds check failed\n",        \
            __FILE__, __LINE__);                          \
    exit(1);                                              \
}                                                         \
}

// Defined in modelaggregator.conf
static const int MAX_TCS = 32;

// Ciphertext, IV, and tag are required for decryption
static const size_t ENCRYPTION_METADATA_LENGTH = 3;

// Global variables stored for threading
static vector<map<string, vector<float>>> g_accumulator;
static vector<string> g_vars_to_aggregate;
static map<string, vector<float>> g_old_params;
static int NUM_THREADS;

// Helper function used to copy double pointers from untrusted memory to enclave memory
void copy_arr_to_enclave(uint8_t* dst[], size_t num, uint8_t* src[], size_t lengths[]) {
  for (int i = 0; i < num; i++) {
    size_t nlen = lengths[i];
    check_host_buffer(src[i], nlen);
    dst[i] = new uint8_t[nlen];
    memcpy((void*) dst[i], (const void*) src[i], nlen);
  }
}

// Stores the unencrypted values needed for aggregation
void enclave_store_globals(uint8_t*** encrypted_accumulator,
            size_t* accumulator_lengths,
            size_t accumulator_length,
            uint8_t** encrypted_old_params,
            size_t old_params_length) {
    set<string> vars;
    // This for loop decrypts the accumulator and adds all
    // variables received by the clients into a set.
    for (int i = 0; i < accumulator_length; i++) {
        // Copy double pointers to enclave memory again
        uint8_t** encrypted_accumulator_i_cpy = new uint8_t*[ENCRYPTION_METADATA_LENGTH * sizeof(uint8_t*)];
        size_t lengths[] = {accumulator_lengths[i] * sizeof(uint8_t), CIPHER_IV_SIZE, CIPHER_TAG_SIZE};
        copy_arr_to_enclave(encrypted_accumulator_i_cpy,
                ENCRYPTION_METADATA_LENGTH,
                encrypted_accumulator[i],
                lengths);

        uint8_t* serialized_accumulator = new uint8_t[accumulator_lengths[i] * sizeof(uint8_t)];
        decrypt_bytes(encrypted_accumulator_i_cpy[0],
                encrypted_accumulator_i_cpy[1],
                encrypted_accumulator_i_cpy[2],
                accumulator_lengths[i],
                &serialized_accumulator);

        map<string, vector<float>> acc_params = deserialize(serialized_accumulator);

        delete_double_ptr(encrypted_accumulator_i_cpy, ENCRYPTION_METADATA_LENGTH);
        delete serialized_accumulator;

        for (const auto& pair : acc_params) {
            if (pair.first != "_contribution" && !(pair.first.rfind("shape", 0) == 0)) {
                vars.insert(pair.first);
            }
        }
        g_accumulator.push_back(acc_params);
    }
    copy(vars.begin(), vars.end(), back_inserter(g_vars_to_aggregate));

    // Store decrypted old params
    uint8_t* encrypted_old_params_cpy[ENCRYPTION_METADATA_LENGTH];
    size_t lengths[] = {old_params_length * sizeof(uint8_t), CIPHER_IV_SIZE, CIPHER_TAG_SIZE};
    copy_arr_to_enclave(encrypted_old_params_cpy,
            ENCRYPTION_METADATA_LENGTH,
            encrypted_old_params,
            lengths);
    uint8_t* serialized_old_params = new uint8_t[old_params_length * sizeof(uint8_t)];
    decrypt_bytes(encrypted_old_params_cpy[0],
            encrypted_old_params_cpy[1],
            encrypted_old_params_cpy[2],
            old_params_length,
            &serialized_old_params);

    g_old_params = deserialize(serialized_old_params);
}

// Validates the number of threads that the host is trying to create
bool enclave_set_num_threads(int num_threads) {
    // We can't run more threads than we have TCSs, and there can't be more threads than weights
    if (num_threads > MAX_TCS || num_threads > g_vars_to_aggregate.size()) {
        return false;
    }
    NUM_THREADS = num_threads;
    return true;
}

// This is the function that the host calls. It performs the aggregation and updates g_old_params.
void enclave_modelaggregator(int tid) {
    // Fast ceiling division of g_vars_to_aggregate.size() / NUM_THREADS
    int slice_length = 1 + ((g_vars_to_aggregate.size() - 1) / NUM_THREADS);

    // Slice the vector depending on thread ID
    int i = tid * slice_length;
    int j = min((int) g_vars_to_aggregate.size(), (tid + 1) * slice_length);

    // Each thread iterates through a portion of all weights names received by the clients.
    for (; i < j; i++) {
        string v_name = g_vars_to_aggregate[i];
        float iters_sum = 0;

        // For each accumulator, we find the vector of the current weight and
        // multiple all of it's elements by local iterations. We keep a running
        // sum of total iterations and a vector of all weights observed.
        for (int k = 0; k < g_accumulator.size(); k++) {
            map<string, vector<float>> acc_params = g_accumulator[k];
            if (acc_params.find(v_name) == acc_params.end()) { // This accumulator doesn't have the given variable
                continue;
            }

            // Each params map will have an additional key "_contribution" to hold the number of local iterations.
            float n_iter = acc_params["_contribution"][0];
            iters_sum += n_iter;

            if (g_old_params[v_name].size() != acc_params[v_name].size()) {
                std::cout << "Error! Unequal sizes" << std::endl;
            }

            __m256d iters_sum_slice;
            if (k == g_accumulator.size() - 1 && iters_sum > 0) {
                const float iters_sum_arr[4] = {iters_sum, iters_sum, iters_sum, iters_sum};
                iters_sum_slice = _mm256_loadu_pd(iters_sum_arr);
            }
            const float n_iter_arr[4] = {n_iter, n_iter, n_iter, n_iter};
            __m256d n_iter_slice = _mm256_loadu_pd(n_iter_arr);

            // Multiple the weights by local iterations and update g_old_params[v_name].
            for (int i = 0; i < acc_params[v_name].size() / 4 * 4; i += 4) {
                __m256d weights_slice = _mm256_loadu_pd((const float*) acc_params[v_name].data() + i);
                __m256d old_params_v_name_slice = _mm256_loadu_pd((const float*) g_old_params[v_name].data() + i);

                __m256d updated_old_params_v_name_slice = _mm256_add_pd(old_params_v_name_slice,
                        _mm256_mul_pd(weights_slice, n_iter_slice));

                if (k == g_accumulator.size() - 1 && iters_sum > 0) {
                    updated_old_params_v_name_slice = _mm256_div_pd(updated_old_params_v_name_slice, iters_sum_slice);
                }

                _mm256_storeu_pd(g_old_params[v_name].data() + i, updated_old_params_v_name_slice);
            }
            // Tail case.
            for (int i = acc_params[v_name].size() / 4 * 4; i < acc_params[v_name].size(); i++) {
                g_old_params[v_name][i] += acc_params[v_name][i] * n_iter;

                if (k == g_accumulator.size() - 1 && iters_sum > 0) {
                    g_old_params[v_name][i] /= iters_sum;
                }
            }
        }
    }
}

void enclave_transfer_model_out(uint8_t*** encrypted_new_params_ptr, size_t* new_params_length) {
    int serialized_buffer_size = 0;
    uint8_t* serialized_new_params = serialize(g_old_params, &serialized_buffer_size);

    uint8_t** encrypted_new_params = new uint8_t*[ENCRYPTION_METADATA_LENGTH * sizeof(uint8_t*)];
    encrypted_new_params[0] = new uint8_t[serialized_buffer_size * sizeof(uint8_t)];
    encrypted_new_params[1] = new uint8_t[CIPHER_IV_SIZE * sizeof(uint8_t)];
    encrypted_new_params[2] = new uint8_t[CIPHER_TAG_SIZE * sizeof(uint8_t)];
    encrypt_bytes(serialized_new_params, serialized_buffer_size, encrypted_new_params);

    // Need to copy the encrypted model, IV, and tag over to untrusted memory.
    *encrypted_new_params_ptr = (uint8_t**) oe_host_malloc(ENCRYPTION_METADATA_LENGTH * sizeof(uint8_t*));
    *new_params_length = serialized_buffer_size;
    size_t item_lengths[3] = {*new_params_length, CIPHER_IV_SIZE, CIPHER_TAG_SIZE};
    for (int i = 0; i < ENCRYPTION_METADATA_LENGTH; i++) {
        (*encrypted_new_params_ptr)[i] = (uint8_t*) oe_host_malloc(item_lengths[i] * sizeof(uint8_t));
        memcpy((void *) (*encrypted_new_params_ptr)[i], (const void*) encrypted_new_params[i], item_lengths[i] * sizeof(uint8_t));
    }

    delete_double_ptr(encrypted_new_params, ENCRYPTION_METADATA_LENGTH);

    // Clear the global variables before the next round of training
    g_accumulator.clear();
    g_vars_to_aggregate.clear();
    g_old_params.clear();
}
