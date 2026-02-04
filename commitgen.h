#pragma once
#include <string>
#include <memory>

class CommitGen {
public:
    CommitGen(const std::string& model_path);
    ~CommitGen();

    bool is_ready() const;
    std::string generate(const std::string& diff);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
