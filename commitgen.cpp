#include "commitgen.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "llama.h"

struct CommitGen::Impl {
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    std::mutex mtx;
    std::atomic<bool> ready{false};
    std::future<void> init_future;
};

CommitGen::CommitGen(const std::string& model_path) : impl(std::make_unique<Impl>()) {
    impl->init_future = std::async(std::launch::async, [this, model_path]() {
// Only set log callback in server mode to avoid client interference
#ifdef SERVER_MODE
        llama_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
#endif

        llama_model_params model_params = llama_model_default_params();
        impl->model = llama_model_load_from_file(model_path.c_str(), model_params);

        if (!impl->model)
            return;

        llama_context_params ctx_params = llama_context_default_params();
        ctx_params.n_ctx = 4096;
        impl->ctx = llama_init_from_model(impl->model, ctx_params);
        impl->vocab = llama_model_get_vocab(impl->model);

        impl->ready = true;
    });
}

CommitGen::~CommitGen() {
    if (impl->init_future.valid()) {
        impl->init_future.wait();
    }
    if (impl->ctx)
        llama_free(impl->ctx);
    if (impl->model)
        llama_model_free(impl->model);
}

bool CommitGen::is_ready() const {
    return impl->ready.load();
}

std::string build_prompt(const std::string& diff) {
    return R"(<|im_start|>system
You are a commit message generator. Write a clear, natural commit message.

Rules:
- First line: short summary of what changed (max 72 chars)
- Then a blank line
- Then a paragraph explaining the changes in plain English
- No prefixes like "feat:", "fix:", etc.
- No bullet points
- No "I" statements - use passive voice or imperative
- Write like documentation, not a personal note

Example:
Disable playground build by default

The CMake configuration now has BUILD_PLAYGROUND disabled by default to streamline the build process. Users who need the playground examples can enable it manually in their local configuration.
<|im_end|>
<|im_start|>user
)" + diff
        + R"(
<|im_end|>
<|im_start|>assistant
)";
}

std::string CommitGen::generate(const std::string& diff) {
    if (!is_ready())
        return "";

    std::lock_guard<std::mutex> lock(impl->mtx);

    // Clear the KV cache
    llama_memory_t mem = llama_get_memory(impl->ctx);
    llama_memory_seq_rm(mem, 0, 0, -1);

    std::string input = diff.substr(0, 4000);
    std::string prompt = build_prompt(input);  // Fixed: was using `diff` instead of `input`
    std::vector<llama_token> tokens(prompt.size() + 16);
    int n_tokens =
        llama_tokenize(impl->vocab, prompt.c_str(), (int)prompt.size(), tokens.data(), (int)tokens.size(), true, true);
    if (n_tokens < 0)
        return "";
    tokens.resize(n_tokens);

    llama_batch batch = llama_batch_get_one(tokens.data(), (int)tokens.size());
    if (llama_decode(impl->ctx, batch) != 0)
        return "";

    llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.3f));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.9f, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(42));

    std::string stop_str = "<|im_end|>";

    std::string result;
    int consecutive_newlines = 0;

    for (int i = 0; i < 512; i++) {  // Increased from 100 to 512
        llama_token new_token = llama_sampler_sample(sampler, impl->ctx, -1);
        if (llama_vocab_is_eog(impl->vocab, new_token))
            break;

        char buf[256];
        int len = llama_token_to_piece(impl->vocab, new_token, buf, sizeof(buf), 0, true);
        if (len < 0)
            continue;

        std::string piece(buf, len);
        result.append(piece);

        // Stop at <|im_end|> token
        if (result.find(stop_str) != std::string::npos) {
            result = result.substr(0, result.find(stop_str));
            break;
        }

        // Stop after 3 consecutive newlines (end of message)
        if (piece == "\n") {
            consecutive_newlines++;
            if (consecutive_newlines >= 3)
                break;
        } else if (piece.find_first_not_of(" \t") != std::string::npos) {
            consecutive_newlines = 0;
        }

        batch = llama_batch_get_one(&new_token, 1);
        if (llama_decode(impl->ctx, batch) != 0)
            break;
    }

    llama_sampler_free(sampler);

    // Clean up result
    // Remove quotes if present
    if (!result.empty() && result[0] == '"')
        result.erase(0, 1);
    if (!result.empty() && result.back() == '"')
        result.pop_back();

    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == ' '))
        result.pop_back();

    return result;
}
