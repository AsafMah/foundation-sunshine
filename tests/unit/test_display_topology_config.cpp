/**
 * @file tests/unit/test_display_topology_config.cpp
 * @brief The `display_topology_profile` key: blank by default, and kept intact by the config file.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

#include "../tests_common.h"
#include <src/config.h>

namespace {
  constexpr std::string_view profile = R"({"version":1,"monitors":[{"source":"physical","display":"\\\\.\\DISPLAY2"},{"source":"virtual"}]})";

  class temporary_topology_config_t {
  public:
    temporary_topology_config_t():
        original_path_(config::sunshine.config_file),
        path_(
          std::filesystem::temp_directory_path() /
          ("sunshine_topology_config_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".conf")
        ) {
      config::sunshine.config_file = path_.string();
    }

    ~temporary_topology_config_t() {
      config::sunshine.config_file = original_path_;
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }

    void
    write(std::string_view content) const {
      std::ofstream file(path_, std::ios::binary | std::ios::trunc);
      file << content;
    }

    std::string
    read() const {
      std::ifstream file(path_, std::ios::binary);
      return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

  private:
    std::string original_path_;
    std::filesystem::path path_;
  };
}  // namespace

TEST(DisplayTopologyConfig, TheProfileIsBlankByDefaultSoTheHostStaysLegacy) {
  EXPECT_TRUE(config::video.display_topology_profile.empty());
}

TEST(DisplayTopologyConfig, AJsonProfileIsReadAsOneValue) {
  const auto vars = config::parse_config("display_topology_profile = " + std::string { profile } + "\r\nsunshine_name = host\r\n");
  ASSERT_TRUE(vars.contains("display_topology_profile"));
  EXPECT_EQ(vars.at("display_topology_profile"), profile);
}

TEST(DisplayTopologyConfig, AWebUiSaveKeepsTheProfile) {
  temporary_topology_config_t file;
  file.write("display_topology_profile = " + std::string { profile } + "\nsunshine_name = old\n");

  // The Web UI posts back every key it loaded, including keys it has no editor for.
  auto loaded = config::parse_config(file.read());
  std::map<std::string, std::string> incoming { loaded.begin(), loaded.end() };
  incoming["sunshine_name"] = "new";
  ASSERT_TRUE(config::update_full_config(incoming));

  const auto persisted = config::parse_config(file.read());
  ASSERT_TRUE(persisted.contains("display_topology_profile"));
  EXPECT_EQ(persisted.at("display_topology_profile"), profile);
  EXPECT_EQ(persisted.at("sunshine_name"), "new");
}

TEST(DisplayTopologyConfig, AWebUiSaveNeverPersistsTheReadOnlyStatus) {
  temporary_topology_config_t file;
  file.write("sunshine_name = old\n");

  // The status block is loaded with the config, so a save posts it back too.
  std::map<std::string, std::string> incoming {
    { "sunshine_name", "new" },
    { "display_topology", R"({"desired":"managed","applied":"legacy","restart_required":"true"})" },
  };
  ASSERT_TRUE(config::update_full_config(incoming));

  const auto persisted = config::parse_config(file.read());
  EXPECT_FALSE(persisted.contains("display_topology"));
  EXPECT_FALSE(persisted.contains("display_topology_profile"));
  EXPECT_EQ(persisted.at("sunshine_name"), "new");
}
