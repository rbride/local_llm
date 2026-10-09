// aliases.hpp - aliases.txt: nicknames that map to real Telegram users.
//
// The file is meant to be edited by hand. The bot reloads it whenever it changes,
// and admin commands (/alias, /unalias) edit it line by line.
//
//   # aliases.txt - nicknames for people. One line per person: <user_id>: name, name, @username
//   123456789: Drew, D, @BrandRiver
//   987654321: Casey, !Dr.Business, @YoMamaLlama
//
// An alias starting with "!" is owner-protected: regular admins can't delete it.
// To keep an alias that really starts with "!", write "\!" before it.
#pragma once
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct AliasInfo {
    std::string name;
    bool protected_ = false;
};

class Aliases {
public:
    explicit Aliases(std::string path);

    void create_if_missing();
    long long resolve(const std::string& name);
    std::vector<std::string> names_for(long long user_id);
    std::vector<AliasInfo> entries(long long user_id);
    std::map<long long, std::vector<AliasInfo>> all();
    bool add(long long user_id, const std::string& name, bool protected_alias, std::string& error);
    bool remove(const std::string& name, bool as_owner, std::string& error);

private:
    struct StoredAlias {
        std::string display;
        std::string match;  // lower-case, leading @ removed
        bool protected_ = false;
    };
    struct Person {
        long long user_id = 0;
        size_t line = 0;
        std::vector<StoredAlias> aliases;
    };

    std::string path_;
    std::mutex mu_;
    std::vector<std::string> lines_;
    std::vector<Person> people_;
    std::time_t mtime_ = 0;

    void reload_locked(bool force = false);
    void parse_locked();
    bool write_locked();
    std::string render_line(const Person& p) const;
    Person* find_person_locked(long long user_id);
    size_t count_aliases_locked(long long user_id);
};
