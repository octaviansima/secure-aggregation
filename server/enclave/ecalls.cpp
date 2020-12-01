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
#include <iostream>

// Include encryption/decryption and serialization/deserialization headers
#include "encryption/encrypt.h"
#include "encryption/serialization.h"
#include "utils.h"
#include "synch.h"

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
static int NUM_THREADS;
static vector<map<string, vector<double>>> g_accumulator;
static set<string> g_vars_to_aggregate;
static map<string, vector<double>> g_old_params;

// Lock to prevent concurrency issues when writing to g_old_params
static Synch::Lock params_lock;

// Helper function used to copy double pointers from untrusted memory to enclave memory
void copy_arr_to_enclave(uint8_t* dst[], size_t num, uint8_t* src[], size_t lengths[]) {
  for (int i = 0; i < num; i++) {
    size_t nlen = lengths[i];
    check_host_buffer(src[i], nlen);
    dst[i] = new uint8_t[nlen];
    memcpy((void*) dst[i], (const void*) src[i], nlen);
  }
}

bool enclave_set_num_threads(int num_threads) {
    // We can't run more threads than we have TCSs
    if (num_threads > MAX_TCS) {
        return false;
    }
    NUM_THREADS = num_threads;
    return true;
}

void enclave_store_globals(uint8_t*** encrypted_accumulator,
            size_t* accumulator_lengths,
            size_t accumulator_length,
            uint8_t** encrypted_old_params,
            size_t old_params_length) {
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

        map<string, vector<double>> acc_params = deserialize(serialized_accumulator);

        delete_double_ptr(encrypted_accumulator_i_cpy, ENCRYPTION_METADATA_LENGTH);
        delete serialized_accumulator;

        for (const auto& pair : acc_params) {
            if (pair.first != "_contribution" && !(pair.first.rfind("shape", 0) == 0)) {
                g_vars_to_aggregate.insert(pair.first);
            }
        }
        g_accumulator.push_back(acc_params);
    }

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

void hello_enclave() {
    fprintf( stderr, "HELLO FROM THE ENCLAVE\n");
}

// This is the function that the host calls. It performs
// the aggregation and encrypts the new model to pass back.
void enclave_modelaggregator(int tid) {
    std::cout << "Enclave: starting model aggregation" << std::endl;
    // We iterate through all weights names received by the clients.
    int i = 0;
    int total = g_vars_to_aggregate.size();
    for (string v_name : g_vars_to_aggregate) {
        if (i %20==0)
            fprintf(stderr, "(%d of %d)\n", i, total);
        i++;

        double iters_sum = 0;
        // vector<vector<double>> vars;
        vector<double> updated_params_at_var(g_old_params[v_name]);
        //if (v_name == "stage9/_dense_block/_pseudo_3d/9c_iter2_conv4/conv3d/kernel:0")
        //  std::cout << "1 - old params" << std::endl;
        //if (v_name == "stage9/_dense_block/_pseudo_3d/9c_iter2_conv4/conv3d/kernel:0") {
        //  for (auto x: updated_params_at_var)
        //    std::cout << x << ", ";
        //  std::cout << std::endl;
        //}

        // For each accumulator, we find the vector of the current weight and
        // multiple all of it's elements by local iterations. We keep a running
        // sum of total iterations and a vector of all weights observed.
        for (map<string, vector<double>> acc_params : g_accumulator) {
            if (acc_params.find(v_name) == acc_params.end()) { // This accumulator doesn't have the given variable
                continue;
            }

            // Each params map will have an additional key "_contribution" to hold the number of local iterations.
            double n_iter = acc_params["_contribution"][0];
            iters_sum += n_iter;

            // Multiple the weights by local iterations.
            vector<double>& weights = acc_params[v_name];
            if (updated_params_at_var.size() != weights.size()) {
                std::cout << "Error! Unequal sizes" << std::endl;
            }

            //if (v_name == "stage9/_dense_block/_pseudo_3d/9c_iter2_conv4/conv3d/kernel:0")
            //  std::cout << "2 - weights" << std::endl;
            for (int i = 0; i < weights.size(); i++) {
                updated_params_at_var[i] += weights[i] * n_iter;
                //if (v_name == "stage9/_dense_block/_pseudo_3d/9c_iter2_conv4/conv3d/kernel:0")
                //  std::cout << i << ", ";
            }
            //if (v_name == "stage9/_dense_block/_pseudo_3d/9c_iter2_conv4/conv3d/kernel:0")
            //  std::cout << "\n n_iter: " << n_iter << std::endl;
        }

        if (iters_sum == 0) {
            continue; // Didn't receive this variable from any clients
        }

        for (int i = 0; i < updated_params_at_var.size(); i++) {
            updated_params_at_var[i] /= iters_sum;
        }
        params_lock.lock();
        g_old_params[v_name] = updated_params_at_var;
        params_lock.unlock();
        //if (v_name == "stage9/_dense_block/_pseudo_3d/9c_iter2_conv4/conv3d/kernel:0")
        //  std::cout << "3" << std::endl;
        //if (v_name == "stage9/_dense_block/_pseudo_3d/9c_iter2_conv4/conv3d/kernel:0") {
        //  for (auto x: updated_params_at_var)
        //    std::cout << x << ", ";
        //  std::cout << std::endl;
        //}
    }
/*
 *    for (string v_name : g_vars_to_aggregate) {
 *        double iters_sum = 0;
 *        vector<vector<double>> vars;
 *
 *        // For each accumulator, we find the vector of the current weight and
 *        // multiple all of it's elements by local iterations. We keep a running
 *        // sum of total iterations and a vector of all weights observed.
 *        for (map<string, vector<double>> acc_params : g_accumulator) {
 *            if (acc_params.find(v_name) == acc_params.end()) { // This accumulator doesn't have the given variable
 *                continue;
 *            }
 *
 *            // Each params map will have an additional key "_contribution" to hold the number of local iterations.
 *            double n_iter = acc_params["_contribution"][0];
 *            iters_sum += n_iter;
 *
 *            // Multiple the weights by local iterations.
 *            vector<double>& weights = acc_params[v_name];
 *            for_each(weights.begin(), weights.end(), [&n_iter](double& d) { d *= n_iter; });
 *            vars.push_back(weights);
 *        }
 *
 *        if (iters_sum == 0) {
 *            continue; // Didn't receive this variable from any clients
 *        }
 *
 *        // Take the element-wise sum of all the weights and add it to the
 *        // old model parameters. Then, divide by the total iterations over
 *        // all clients that had this weight.
 *        for (int i = 0; i < g_old_params[v_name].size(); i++) {
 *            for (vector<double> weights : vars) {
 *                g_old_params[v_name][i] += weights[i];
 *            }
 *            g_old_params[v_name][i] /= iters_sum;
 *        }
 *    }
 */
    //for (auto x :g_old_params["stage9/_dense_block/_pseudo_3d/9c_iter2_conv4/conv3d/kernel:0"])
    //  std::cout << x << ", ";
    //std::cout << std::endl;
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
    std::cout << "Enclave: model encryption done" << std::endl;

    delete_double_ptr(encrypted_new_params, ENCRYPTION_METADATA_LENGTH);
}
