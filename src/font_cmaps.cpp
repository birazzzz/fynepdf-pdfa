#include "font_cmaps.h"

#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace fynepdf::pdfa::font {
namespace {

std::mutex mutex;
std::string folder;
std::map<std::string, std::shared_ptr<const std::string>> read;  // by name; null: not there

} // namespace

void SetCMapFolder(const std::string &path) {
  std::lock_guard<std::mutex> lock(mutex);
  if (path != folder) {
    folder = path;
    read.clear();
  }
}

std::shared_ptr<const std::string> PredefinedCMap(const std::string &name) {
  // A name from a PDF: never a path.
  if (name.empty() || name.size() > 64)
    return nullptr;
  for (const char c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-')
      return nullptr;
  std::lock_guard<std::mutex> lock(mutex);
  if (folder.empty())
    return nullptr;
  const auto found = read.find(name);
  if (found != read.end())
    return found->second;
  std::ifstream in(folder + "/" + name, std::ios::binary);
  std::shared_ptr<const std::string> program;
  if (in) {
    std::ostringstream data;
    data << in.rdbuf();
    program = std::make_shared<const std::string>(data.str());
  }
  read.emplace(name, program);
  return program;
}

} // namespace fynepdf::pdfa::font
