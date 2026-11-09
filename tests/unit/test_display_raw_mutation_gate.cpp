/**
 * @file tests/unit/test_display_raw_mutation_gate.cpp
 * @brief Source gate: every raw Windows display or VDD write in src/ is reachable only behind a topology mutation capability.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
  // Every identifier that writes display configuration, sends a VDD command or launches the adapter-restart helper.
  const std::set<std::string> primitives {
    "SetDisplayConfig",
    "ChangeDisplaySettings",
    "ChangeDisplaySettingsA",
    "ChangeDisplaySettingsW",
    "ChangeDisplaySettingsEx",
    "ChangeDisplaySettingsExA",
    "ChangeDisplaySettingsExW",
    "DisplayConfigSetDeviceInfo",
    "DeviceIoControl",
    "IOCTL_VDD_COMMAND",
    "make_owned_actor_process",
    "adapter_restart_command",
    // Turns a process factory (adapter_restart_plan_t::process) into a launchable actor body.
    "make_process_actor_body",
  };

  struct allowance_t {
    std::string file;
    std::string function;
    std::string primitive;
    std::vector<std::string> required;
    std::vector<std::string> forbidden;
    const char *reason;
    // Applies only to a capability-gated use that is not a direct call (a pass to a synchronous seam).
    bool indirect = false;
  };

  struct violation_t {
    std::string file;
    std::size_t line;
    std::string primitive;
    std::string function;
    std::string why;
  };

  bool
  ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  }

  /** Blanks comments, literals and (unless kept) preprocessor lines, keeping offsets and newlines. */
  std::string
  strip(const std::string &s, bool keep_preprocessor = false) {
    std::string out = s;
    const auto blank = [&](std::size_t from, std::size_t to) {
      for (auto i = from; i < to && i < out.size(); ++i) {
        if (out[i] != '\n') {
          out[i] = ' ';
        }
      }
    };
    std::size_t i = 0;
    bool line_start = true;
    while (i < s.size()) {
      const char c = s[i];
      if (line_start && c == '#' && !keep_preprocessor) {
        auto j = i;
        while (j < s.size() && !(s[j] == '\n' && (j == 0 || s[j - 1] != '\\'))) {
          ++j;
        }
        blank(i, j);
        i = j;
        continue;
      }
      if (c == '\n') {
        line_start = true;
        ++i;
        continue;
      }
      if (!std::isspace(static_cast<unsigned char>(c))) {
        line_start = false;
      }
      if (s.compare(i, 2, "//") == 0) {
        auto j = s.find('\n', i);
        j = j == std::string::npos ? s.size() : j;
        blank(i, j);
        i = j;
      }
      else if (s.compare(i, 2, "/*") == 0) {
        auto j = s.find("*/", i + 2);
        j = j == std::string::npos ? s.size() : j + 2;
        blank(i, j);
        i = j;
      }
      else if (c == '"' && i > 0 && s[i - 1] == 'R') {
        const auto paren = s.find('(', i);
        const auto delimiter = ")" + s.substr(i + 1, paren - i - 1) + "\"";
        auto j = s.find(delimiter, paren);
        j = j == std::string::npos ? s.size() : j + delimiter.size();
        blank(i + 1, j - 1);
        i = j;
      }
      else if (c == '"' || (c == '\'' && !(i > 0 && ident_char(s[i - 1]) && i + 1 < s.size() && ident_char(s[i + 1])))) {
        auto j = i + 1;
        while (j < s.size() && s[j] != c && s[j] != '\n') {
          j += s[j] == '\\' ? 2 : 1;
        }
        blank(i + 1, j);
        i = j + 1;
      }
      else {
        ++i;
      }
    }
    return out;
  }

  std::string
  trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
  }

  /** Strips trailing qualifiers so a definition head ends with its closing parenthesis. */
  std::string
  without_qualifiers(std::string head) {
    static const std::vector<std::string> qualifiers { "const", "noexcept", "override", "final", "mutable" };
    for (bool changed = true; changed;) {
      changed = false;
      head = trim(head);
      for (const auto &q : qualifiers) {
        if (head.size() > q.size() && head.ends_with(q) && !ident_char(head[head.size() - q.size() - 1])) {
          head.resize(head.size() - q.size());
          changed = true;
        }
      }
      const auto arrow = head.rfind("->");
      if (arrow != std::string::npos && head.find(')', arrow) == std::string::npos) {
        head.resize(arrow);
        changed = true;
      }
    }
    return head;
  }

  struct function_t {
    std::size_t head_begin;
    std::size_t open;
    std::size_t close;
    std::string head;
    std::string name;
    bool lambda;
  };

  std::string
  function_name(const std::string &head) {
    int depth = 0;
    auto i = head.size();
    while (i > 0) {
      --i;
      if (head[i] == ')') {
        ++depth;
      }
      else if (head[i] == '(' && --depth == 0) {
        break;
      }
    }
    auto end = i;
    while (end > 0 && std::isspace(static_cast<unsigned char>(head[end - 1]))) {
      --end;
    }
    auto begin = end;
    while (begin > 0 && (ident_char(head[begin - 1]) || head[begin - 1] == ':' || head[begin - 1] == '~')) {
      --begin;
    }
    auto name = head.substr(begin, end - begin);
    const auto scope = name.rfind("::");
    return name.empty() ? std::string { "<lambda>" } : scope == std::string::npos ? name : name.substr(scope + 2);
  }

  /** Every function and lambda body, nested ones included; class, namespace, control and initializer braces are not functions. */
  std::vector<function_t>
  functions(const std::string &t) {
    struct open_t {
      std::size_t head_begin;
      std::size_t open;
      bool function;
      std::string head;
    };
    std::vector<open_t> stack;
    std::vector<function_t> result;
    std::size_t boundary = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
      if (t[i] == '{') {
        const auto head = trim(t.substr(boundary, i - boundary));
        const auto bare = without_qualifiers(head);
        bool function = bare.ends_with(')') || bare.ends_with(']');
        for (const auto *control : { "if", "for", "while", "switch", "catch" }) {
          const std::string word = control;
          if (bare.starts_with(word) && (bare.size() == word.size() || !ident_char(bare[word.size()]))) {
            function = false;
          }
        }
        stack.push_back({ boundary, i, function, head });
        boundary = i + 1;
      }
      else if (t[i] == '}') {
        if (!stack.empty()) {
          const auto top = stack.back();
          stack.pop_back();
          if (top.function) {
            const auto bare = without_qualifiers(top.head);
            const auto name = bare.ends_with(']') ? std::string { "<lambda>" } : function_name(bare);
            result.push_back({ top.head_begin, top.open, i, top.head, name, name == "<lambda>" });
          }
        }
        boundary = i + 1;
      }
      else if (t[i] == ';') {
        boundary = i + 1;
      }
    }
    return result;
  }

  /** Names of the parameters whose type is exactly `type` (optionally namespace-qualified), from the head's own parameter list. */
  std::vector<std::string>
  parameters_of_type(const std::string &head, const std::string &type) {
    const auto bare = without_qualifiers(head);
    if (!bare.ends_with(')')) {
      return {};
    }
    int depth = 0;
    auto open = bare.size();
    while (open > 0) {
      --open;
      if (bare[open] == ')') {
        ++depth;
      }
      else if (bare[open] == '(' && --depth == 0) {
        break;
      }
    }
    std::vector<std::string> params;
    std::string current;
    depth = 0;
    for (auto i = open + 1; i + 1 < bare.size(); ++i) {
      const char c = bare[i];
      depth += (c == '(' || c == '<' || c == '[' || c == '{') ? 1 : (c == ')' || c == '>' || c == ']' || c == '}') ? -1 : 0;
      if (c == ',' && depth == 0) {
        params.push_back(current);
        current.clear();
      }
      else {
        current += c;
      }
    }
    params.push_back(current);
    std::vector<std::string> names;
    for (auto param : params) {
      param = trim(param.substr(0, param.find('=')));
      auto end = param.size();
      auto begin = end;
      while (begin > 0 && ident_char(param[begin - 1])) {
        --begin;
      }
      const auto name = param.substr(begin, end - begin);
      const auto declared = param.substr(0, begin);
      for (auto at = declared.find(type); at != std::string::npos; at = declared.find(type, at + 1)) {
        const auto after = at + type.size();
        if ((at == 0 || !ident_char(declared[at - 1])) && (after >= declared.size() || !ident_char(declared[after])) && !name.empty()) {
          names.push_back(name);
          break;
        }
      }
    }
    return names;
  }

  // Whole-token match: `cap.require(` must not match `other_cap.require(`, `holder.cap.require(`,
  // `holder->cap.require(` or `ns::cap.require(`, and `SDC_APPLY` must not match `SDC_APPLY_X`.
  bool
  token_at(const std::string &text, const std::string &token, std::size_t at) {
    if (ident_char(token.front()) && at > 0) {
      const char prev = text[at - 1];
      if (ident_char(prev) || prev == '.' || (prev == '>' && at > 1 && text[at - 2] == '-') || (prev == ':' && at > 1 && text[at - 2] == ':')) {
        return false;
      }
    }
    const auto after = at + token.size();
    return !(ident_char(token.back()) && after < text.size() && ident_char(text[after]));
  }

  bool
  contains(const std::string &text, const std::string &token) {
    if (token.empty()) {
      return true;
    }
    for (auto at = text.find(token); at != std::string::npos; at = text.find(token, at + 1)) {
      if (token_at(text, token, at)) {
        return true;
      }
    }
    return false;
  }

  std::size_t
  previous_non_space(const std::string &t, std::size_t i) {
    while (i > 0 && std::isspace(static_cast<unsigned char>(t[i - 1]))) {
      --i;
    }
    return i == 0 ? std::string::npos : i - 1;
  }

  char
  next_non_space_char(const std::string &t, std::size_t i) {
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) {
      ++i;
    }
    return i < t.size() ? t[i] : '\0';
  }

  /**
   * True when `cap.require(` in `before` (closed blocks already blanked) always runs: it starts a statement
   * (after `{`, `;`, `}` or the body's opening) outside any parentheses, so no braceless control head, label,
   * operator or for-header governs it.
   */
  bool
  requires_unconditionally(const std::string &before, const std::string &cap) {
    const auto token = cap + ".require(";
    for (auto at = before.find(token); at != std::string::npos; at = before.find(token, at + 1)) {
      if (!token_at(before, token, at)) {
        continue;
      }
      int depth = 0;
      for (std::size_t i = 0; i < at; ++i) {
        depth += before[i] == '(' ? 1 : before[i] == ')' ? -1 : 0;
      }
      const auto prev = previous_non_space(before, at);
      if (depth == 0 && (prev == std::string::npos || before[prev] == '{' || before[prev] == ';' || before[prev] == '}')) {
        return true;
      }
    }
    return false;
  }

  bool
  member_accessed(const std::string &t, std::size_t at) {
    const auto prev = previous_non_space(t, at);
    return prev != std::string::npos && (t[prev] == '.' || (t[prev] == '>' && prev > 0 && t[prev - 1] == '-'));
  }

  /**
   * True when the identifier at [at, end) is called: `f()`, `f.operator()()`, or `std::invoke(f` / `std::invoke(x.f`.
   * Only zero-argument calls count, matching actor_process_factory_t, so `filter->process(input)` is not one.
   */
  bool
  invoked(const std::string &t, std::size_t at, std::size_t end) {
    auto j = end;
    while (j < t.size() && std::isspace(static_cast<unsigned char>(t[j]))) {
      ++j;
    }
    if (j < t.size() && t[j] == '(') {
      auto k = j + 1;
      while (k < t.size() && std::isspace(static_cast<unsigned char>(t[k]))) {
        ++k;
      }
      return k < t.size() && t[k] == ')';
    }
    if (j < t.size() && (t[j] == '.' || t.compare(j, 2, "->") == 0)) {
      j += t[j] == '.' ? 1 : 2;
      // Exactly `operator ( ) ( )`; `operator bool()` and `operator=(` are not calls of the factory.
      for (const auto *piece : { "operator", "(", ")", "(", ")" }) {
        while (j < t.size() && std::isspace(static_cast<unsigned char>(t[j]))) {
          ++j;
        }
        const std::string p = piece;
        if (t.compare(j, p.size(), p) != 0 || (ident_char(p.back()) && j + p.size() < t.size() && ident_char(t[j + p.size()]))) {
          return false;
        }
        j += p.size();
      }
      return true;
    }
    // Walk back over the member-access chain (`a.b->f`) to the opening of an enclosing invoke call.
    auto begin = at;
    while (member_accessed(t, begin)) {
      auto p = previous_non_space(t, begin);
      p = t[p] == '.' ? p : p - 1;
      auto q = previous_non_space(t, p);
      if (q == std::string::npos || !ident_char(t[q])) {
        return false;
      }
      while (q > 0 && ident_char(t[q - 1])) {
        --q;
      }
      begin = q;
    }
    const auto paren = previous_non_space(t, begin);
    if (paren == std::string::npos || t[paren] != '(') {
      return false;
    }
    const auto name_end = previous_non_space(t, paren);
    if (name_end == std::string::npos || !ident_char(t[name_end])) {
      return false;
    }
    auto name_begin = name_end;
    while (name_begin > 0 && ident_char(t[name_begin - 1])) {
      --name_begin;
    }
    const auto name = t.substr(name_begin, name_end - name_begin + 1);
    return name == "invoke";
  }

  /** Every identifier declared with type actor_process_factory_t (members, parameters, locals), across all sources. */
  std::set<std::string>
  factory_names(const std::map<std::string, std::string> &sources) {
    static const std::string type = "actor_process_factory_t";
    std::set<std::string> names;
    for (const auto &[file, source] : sources) {
      const auto t = strip(source);
      for (auto at = t.find(type); at != std::string::npos; at = t.find(type, at + 1)) {
        if (!token_at(t, type, at) && !(at > 1 && t[at - 1] == ':' && t[at - 2] == ':')) {
          continue;
        }
        auto i = at + type.size();
        const auto skip = [&] {
          while (i < t.size() && (std::isspace(static_cast<unsigned char>(t[i])) || t[i] == '&' || t[i] == '*')) {
            ++i;
          }
        };
        skip();
        if (t.compare(i, 5, "const") == 0 && i + 5 < t.size() && !ident_char(t[i + 5])) {
          i += 5;
          skip();
        }
        auto e = i;
        while (e < t.size() && ident_char(t[e])) {
          ++e;
        }
        // `T f(...)` declares a function returning a factory, not a stored one.
        if (e == i || next_non_space_char(t, e) == '(') {
          continue;
        }
        names.insert(t.substr(i, e - i));
        // Later declarators of the same declaration (`T a, b;`); a parameter list's next `type name` stops the walk.
        while (next_non_space_char(t, e) == ',') {
          i = t.find(',', e) + 1;
          while (i < t.size() && (std::isspace(static_cast<unsigned char>(t[i])) || t[i] == '&' || t[i] == '*')) {
            ++i;
          }
          e = i;
          while (e < t.size() && ident_char(t[e])) {
            ++e;
          }
          const char next = next_non_space_char(t, e);
          if (e == i || (next != ',' && next != ';' && next != '=' && next != '{')) {
            break;
          }
          names.insert(t.substr(i, e - i));
        }
      }
    }
    return names;
  }

  struct scan_t {
    std::vector<violation_t> violations;
    std::map<std::string, int> seen;
    std::set<std::size_t> used_allowances;
    std::map<std::size_t, std::size_t> allowance_lines;
  };

  char
  next_non_space(const std::string &t, std::size_t i) {
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) {
      ++i;
    }
    return i < t.size() ? t[i] : '\0';
  }

  /** True for `TYPE name(` at namespace or class scope: a declaration, not a call. */
  bool
  declares(const std::string &t, std::size_t at, std::size_t end) {
    if (next_non_space(t, end) != '(') {
      return false;
    }
    auto statement_begin = t.find_last_of(";{}", at);
    statement_begin = statement_begin == std::string::npos ? 0 : statement_begin + 1;
    const auto before = trim(t.substr(statement_begin, at - statement_begin));
    if (before.empty() || before.find('=') != std::string::npos || before.find('(') != std::string::npos) {
      return false;
    }
    const char last = before.back();
    if (!ident_char(last) && last != '*' && last != '&' && last != '>') {
      return false;
    }
    auto word_begin = before.size();
    while (word_begin > 0 && ident_char(before[word_begin - 1])) {
      --word_begin;
    }
    static const std::set<std::string> not_types { "return", "co_return", "co_await", "co_yield", "throw", "case", "else", "do", "new", "delete", "sizeof" };
    return !not_types.contains(before.substr(word_begin));
  }

  void
  scan_file(const std::string &file, const std::string &source, const std::vector<allowance_t> &allowances, const std::set<std::string> &factories,
    scan_t &scan) {
    const auto t = strip(source);
    const auto with_directives = strip(source, true);
    std::vector<function_t> fs;
    bool parsed = false;
    for (std::size_t i = 0; i < with_directives.size();) {
      if (!ident_char(with_directives[i]) || (i > 0 && ident_char(with_directives[i - 1]))) {
        ++i;
        continue;
      }
      auto end = i;
      while (end < with_directives.size() && ident_char(with_directives[end])) {
        ++end;
      }
      const auto word = with_directives.substr(i, end - i);
      const auto at = i;
      i = end;
      bool factory = false;
      if (!primitives.contains(word)) {
        // A stored process factory launches when called. Names are matched by spelling, so any unqualified call of one fails closed.
        if (!factories.contains(word) || !invoked(with_directives, at, end) ||
            (!member_accessed(with_directives, at) && at > 1 && with_directives[at - 1] == ':' && with_directives[at - 2] == ':')) {
          continue;
        }
        factory = true;
      }
      const auto line = static_cast<std::size_t>(std::count(t.begin(), t.begin() + at, '\n')) + 1;
      if (t[at] != with_directives[at]) {
        // A macro body hides the call from the function scan; only the macro naming the primitive itself is allowed.
        auto line_begin = with_directives.rfind('\n', at);
        line_begin = line_begin == std::string::npos ? 0 : line_begin + 1;
        std::string directive;
        for (auto c : with_directives.substr(line_begin, at - line_begin)) {
          if (!std::isspace(static_cast<unsigned char>(c))) {
            directive += c;
          }
        }
        if (directive != "#define") {
          scan.violations.push_back({ file, line, word, "<none>", "inside a preprocessor directive" });
        }
        continue;
      }
      if (!parsed) {
        fs = functions(t);
        parsed = true;
      }
      // Innermost first; a lambda is governed by its own body and may use only the capabilities it can see.
      std::vector<const function_t *> chain;
      for (const auto &f : fs) {
        if (f.open < at && at < f.close) {
          chain.push_back(&f);
        }
      }
      std::sort(chain.begin(), chain.end(), [](const auto *a, const auto *b) { return a->open > b->open; });
      if (chain.empty()) {
        const auto defined_here = std::any_of(fs.begin(), fs.end(), [&](const auto &f) { return f.head_begin <= at && at < f.open && f.name == word; });
        if (!defined_here && !declares(t, at, end)) {
          scan.violations.push_back({ file, line, word, "<none>", "outside a recognised function body" });
        }
        continue;
      }
      ++scan.seen[word];
      const auto &f = *chain.front();
      const bool direct = factory || word == "IOCTL_VDD_COMMAND" || next_non_space(t, end) == '(';
      const function_t *owner = &f;
      std::vector<std::string> capabilities;
      std::vector<std::string> envelopes;
      for (const auto *g : chain) {
        for (const auto &name : parameters_of_type(g->head, "mutation_capability_t")) {
          capabilities.push_back(name);
        }
        for (const auto &name : parameters_of_type(g->head, "encoded_command_t")) {
          envelopes.push_back(name);
        }
        owner = g;
        if (!g->lambda) {
          break;
        }
      }
      // A require() inside a block that closed before the write (a branch, a nested lambda) does not dominate it.
      // The closing brace is kept so a statement following the block still starts a statement.
      auto before = t.substr(f.open, at - f.open);
      for (std::size_t k = 1, depth = 0, start = 0; k < before.size(); ++k) {
        if (before[k] == '{' && depth++ == 0) {
          start = k;
        }
        else if (before[k] == '}' && depth > 0 && --depth == 0) {
          std::fill(before.begin() + start, before.begin() + k, ' ');
        }
      }
      // A preceding goto can jump over any require to a later label, so it voids them all.
      const bool jumps = contains(t.substr(f.open, at - f.open), "goto");
      const bool gated = !jumps && (std::any_of(capabilities.begin(), capabilities.end(), [&](const auto &c) { return requires_unconditionally(before, c); }) ||
                         std::any_of(envelopes.begin(), envelopes.end(), [&](const auto &e) { return contains(before, e + ".authorized_bytes()"); }));
      if (gated && direct) {
        continue;
      }
      const auto body = t.substr(owner->open, owner->close - owner->open);
      bool allowed = false;
      std::string why = direct ? "no capability.require() or authorized envelope precedes it" : "not a direct call";
      for (std::size_t a = 0; a < allowances.size(); ++a) {
        const auto &allowance = allowances[a];
        if (!file.ends_with(allowance.file) || allowance.function != owner->name || allowance.primitive != word || allowance.indirect == direct ||
            (allowance.indirect && !gated)) {
          continue;
        }
        const auto missing = std::find_if(allowance.required.begin(), allowance.required.end(), [&](const auto &r) { return !contains(body, r); });
        const auto present = std::find_if(allowance.forbidden.begin(), allowance.forbidden.end(), [&](const auto &r) { return contains(body, r); });
        if (missing != allowance.required.end()) {
          why = "allowance requires " + *missing;
        }
        else if (present != allowance.forbidden.end()) {
          why = "allowance forbids " + *present;
        }
        else if (scan.allowance_lines.contains(a)) {
          why = "allowance already used at line " + std::to_string(scan.allowance_lines.at(a));
        }
        else {
          allowed = true;
          scan.used_allowances.insert(a);
          scan.allowance_lines.emplace(a, line);
        }
      }
      if (!allowed) {
        scan.violations.push_back({ file, line, word, f.name, why });
      }
    }
  }
  std::string
  describe(const std::vector<violation_t> &violations) {
    std::ostringstream out;
    for (const auto &v : violations) {
      out << v.file << ':' << v.line << ' ' << v.primitive << " in " << v.function << ": " << v.why << '\n';
    }
    return out.str();
  }

  // Each entry is a non-mutating use or an inert plan; required/forbidden tokens keep the justification true.
  const std::vector<allowance_t> production_allowances {
    { "platform/windows/display_device/native_edge.cpp", "validate_arrange", "SetDisplayConfig", { "SDC_VALIDATE" }, { "SDC_APPLY" },
      "Validate-only: the read-only probe asks Windows whether the planned arrange is valid and writes nothing." },
    { "platform/windows/display_device/windows_utils.cpp", "test_no_access_to_ccd_api", "SetDisplayConfig", { "SDC_VALIDATE" }, { "SDC_APPLY" },
      "Validate-only probe of CCD access; it never applies a configuration." },
    { "platform/windows/display_device/windows_utils.cpp", "set_hdr_state", "DisplayConfigSetDeviceInfo", { "hdr_state_api::set(" }, {},
      "Gated pass to the synchronous hdr_state_api::set template seam, which calls it before returning and stores nothing.", true },
    { "display_device/vdd_ioctl.cpp", "ping", "DeviceIoControl", { "IOCTL_VDD_PING" }, { "IOCTL_VDD_COMMAND" }, "Liveness read." },
    { "display_device/vdd_ioctl.cpp", "query_frame_channel_caps", "DeviceIoControl", { "IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS" }, { "IOCTL_VDD_COMMAND" },
      "Capability read." },
    { "display_device/vdd_ioctl.cpp", "open_frame_channel", "DeviceIoControl", { "IOCTL_VDD_OPEN_FRAME_CHANNEL" }, { "IOCTL_VDD_COMMAND" },
      "Duplicates a frame channel; changes no display or monitor." },
    { "display_device/vdd_utils.cpp", "make_adapter_restart_plan", "make_owned_actor_process", { "plan.process" }, {},
      "Builds an unlaunched plan; topology::restart_adapter requires adapter_restart before launching it." },
    { "display_device/vdd_utils.cpp", "make_adapter_restart_plan", "adapter_restart_command", { "plan.process" }, {},
      "Command text for the same inert plan." },
    { "display_device/topology_actor_body.cpp", "run_process", "make", { "context.retain(process)" }, {},
      "Runs only inside the actor body built by make_process_actor_body, which is itself a gated primitive." },
  };

  scan_t
  scan_sources(const std::map<std::string, std::string> &sources, const std::vector<allowance_t> &allowances) {
    scan_t scan;
    const auto factories = factory_names(sources);
    for (const auto &[file, source] : sources) {
      scan_file(file, source, allowances, factories, scan);
    }
    return scan;
  }
}  // namespace

TEST(DisplayRawMutationGateScanner, CapabilityRequiredBeforeTheWriteIsAccepted) {
  const auto scan = scan_sources({ { "a.cpp", R"(
    bool apply(const topology::mutation_capability_t &cap, int x) {
      cap.require(topology::operation_e::write_topology);
      return SetDisplayConfig(0, nullptr, 0, nullptr, 0) == 0;
    })" } },
    {});
  EXPECT_TRUE(scan.violations.empty()) << describe(scan.violations);
  EXPECT_EQ(scan.seen.at("SetDisplayConfig"), 1);
}

TEST(DisplayRawMutationGateScanner, UngatedOrLateGatedWritesAreRejected) {
  const auto scan = scan_sources({ { "a.cpp", R"(
    namespace n {
      bool raw(int x) { return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) == 0; }
      bool late(const topology::mutation_capability_t &cap) {
        DisplayConfigSetDeviceInfo(nullptr);
        cap.require(topology::operation_e::scale);
        return true;
      }
      bool other(const topology::mutation_capability_t &cap, const topology::mutation_capability_t &unused) {
        if (unused.allows()) { other_object.require(1); }
        return SetDisplayConfig(0, nullptr, 0, nullptr, 0) == 0;
      }
      bool suffix(const topology::mutation_capability_t &cap) {
        other_cap.require(topology::operation_e::rotate);
        holder.cap.require(topology::operation_e::rotate);
        holder->cap.require(topology::operation_e::rotate);
        ns::cap.require(topology::operation_e::rotate);
        return SetDisplayConfig(0, nullptr, 0, nullptr, 0) == 0;
      }
    })" } },
    {});
  ASSERT_EQ(scan.violations.size(), 4u) << describe(scan.violations);
  EXPECT_EQ(scan.violations[0].function, "raw");
  EXPECT_EQ(scan.violations[1].function, "late");
  EXPECT_EQ(scan.violations[2].function, "other");
  EXPECT_EQ(scan.violations[3].function, "suffix");
}

TEST(DisplayRawMutationGateScanner, CommentsStringsDeclarationsAndDefinitionNamesAreNotUses) {
  const auto scan = scan_sources({ { "a.cpp", R"SRC(
    // SetDisplayConfig in a comment
    /* DeviceIoControl in a block comment */
    #define IOCTL_VDD_COMMAND \
      CTL_CODE(1, 2, 3, 4)
    BOOL SetDisplayConfig(UINT32 a, int *b);
    std::wstring adapter_restart_command();
    process_t make_owned_actor_process(spec_t spec);
    process_t make_owned_actor_process(spec_t spec) { return launch(spec); }
    void log() { write("SetDisplayConfig failed"); write(R"(DeviceIoControl)"); char q = '"'; }
  )SRC" } },
    {});
  EXPECT_TRUE(scan.violations.empty()) << describe(scan.violations);
  EXPECT_TRUE(scan.seen.empty());
}

TEST(DisplayRawMutationGateScanner, EnvelopeAuthorizationIsAcceptedAndForgedEnvelopesAreRejected) {
  const auto scan = scan_sources({ { "a.cpp", R"(
    result send(const encoded_command_t &request) {
      const auto &bytes = request.authorized_bytes();
      return DeviceIoControl(h, IOCTL_VDD_COMMAND, bytes, 0, nullptr, 0, nullptr, nullptr);
    }
    result forged(const encoded_command_t &request) {
      return DeviceIoControl(h, IOCTL_VDD_COMMAND, nullptr, 0, nullptr, 0, nullptr, nullptr);
    })" } },
    {});
  ASSERT_EQ(scan.violations.size(), 2u) << describe(scan.violations);
  EXPECT_EQ(scan.violations[0].function, "forged");
  EXPECT_EQ(scan.violations[1].function, "forged");
}

// A lambda can run later, after its creator's require(); it must authorize inside its own body.
TEST(DisplayRawMutationGateScanner, LambdasMustRequireInsideTheirOwnBody) {
  const auto scan = scan_sources({ { "a.cpp", R"(
    void escapes(const topology::mutation_capability_t &cap) {
      cap.require(topology::operation_e::rotate);
      callback = [&](int a) { if (a) { ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr); } };
    }
    void inside(const topology::mutation_capability_t &cap) {
      callback = [&cap](int a) { cap.require(topology::operation_e::rotate); ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr); };
    }
    void hidden(const topology::mutation_capability_t &cap) {
      auto never = [&] { cap.require(topology::operation_e::rotate); };
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void branch(const topology::mutation_capability_t &cap, bool invalid) {
      if (invalid) { cap.require(topology::operation_e::adapter_restart); return; }
      make_process_actor_body(std::move(factory));
    })" } },
    {});
  ASSERT_EQ(scan.violations.size(), 3u) << describe(scan.violations);
  EXPECT_EQ(scan.violations[0].function, "<lambda>");
  EXPECT_EQ(scan.violations[0].line, 4u);
  EXPECT_EQ(scan.violations[1].function, "hidden");
  EXPECT_EQ(scan.violations[2].function, "branch");
}

// Only a require() that runs unconditionally dominates: a full statement, outside parentheses and not under a braceless control head.
TEST(DisplayRawMutationGateScanner, ConditionalRequiresWithoutBracesDoNotDominate) {
  const auto scan = scan_sources({ { "a.cpp", R"(
    void braceless(const topology::mutation_capability_t &cap, bool x) {
      if (x) cap.require(topology::operation_e::rotate);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void otherwise(const topology::mutation_capability_t &cap, bool x) {
      if (x) { log(); } else cap.require(topology::operation_e::rotate);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void loop(const topology::mutation_capability_t &cap, int n) {
      for (int i = 0; i < n; ++i) cap.require(topology::operation_e::rotate);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void looped(const topology::mutation_capability_t &cap, int n) {
      while (n--) cap.require(topology::operation_e::rotate);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void done(const topology::mutation_capability_t &cap) {
      do cap.require(topology::operation_e::rotate); while (false);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void shortcut(const topology::mutation_capability_t &cap, bool x) {
      x && (cap.require(topology::operation_e::rotate), true);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void chosen(const topology::mutation_capability_t &cap, bool x) {
      x ? cap.require(topology::operation_e::rotate) : skip();
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void stepped(const topology::mutation_capability_t &cap) {
      for (;; cap.require(topology::operation_e::rotate)) { break; }
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void labelled(const topology::mutation_capability_t &cap, int x) {
      switch (x) { case 1: cap.require(topology::operation_e::rotate); case 2: SetDisplayConfig(0, nullptr, 0, nullptr, 0); }
    }
    void jumped(const topology::mutation_capability_t &cap, bool skip) {
      if (skip) goto raw;
      cap.require(topology::operation_e::rotate);
    raw:
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void after_branch(const topology::mutation_capability_t &cap, bool x) {
      if (x) { log(); } else { other(); }
      cap.require(topology::operation_e::rotate);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void nested(const topology::mutation_capability_t &cap, bool x) {
      if (x) { cap.require(topology::operation_e::rotate); SetDisplayConfig(0, nullptr, 0, nullptr, 0); }
    })" } },
    {});
  std::vector<std::string> rejected;
  for (const auto &v : scan.violations) {
    rejected.push_back(v.function);
  }
  EXPECT_EQ(rejected, (std::vector<std::string> { "braceless", "otherwise", "loop", "looped", "done", "shortcut", "chosen", "stepped", "labelled", "jumped" }))
    << describe(scan.violations);
}

// A stored actor_process_factory_t launches a process when invoked, whatever the spelling of the call.
// Names are matched by spelling, so a bare call of any factory name fails closed even where it is not the factory.
TEST(DisplayRawMutationGateScanner, StoredProcessFactoryInvocationsNeedTheCapability) {
  const auto scan = scan_sources({ { "plan.h", R"(
    struct plan_t {
      topology::actor_process_factory_t fallback, process;
    };
    topology::actor_process_factory_t choose();)" },
    { "a.cpp", R"(
    void direct(plan_t &plan) { plan.process(); }
    void pointer(plan_t *plan) { auto p = plan->process ( ); }
    void spelled(plan_t &plan) { plan.process.operator ( ) ( ); }
    void invoked(plan_t &plan) { std::invoke(plan.process); }
    void second(plan_t &plan) { plan.fallback(); }
    void param(const topology::actor_process_factory_t &make) { auto p = make(); }
    void param_spelled(topology::actor_process_factory_t make) { make.operator()(); }
    void local() {
      topology::actor_process_factory_t launch = choose();
      launch();
    }
    struct owner_t {
      topology::actor_process_factory_t member;
      void run() { member(); }
    };
    void gated(const topology::mutation_capability_t &cap, plan_t &plan) {
      cap.require(topology::operation_e::adapter_restart);
      plan.process();
    }
    void inert(plan_t &plan, plan_t &other, filter_t *filter) {
      if (!plan.process) return;
      if (plan.process.operator bool()) log();
      plan.process.operator=(other.process);
      auto moved = std::move(plan.process);
      auto a = handle_t::make();
      auto c = filter->process(input);
      auto d = choose();
    })" } },
    {});
  std::vector<std::string> rejected;
  for (const auto &v : scan.violations) {
    rejected.push_back(v.function + ':' + v.primitive);
  }
  EXPECT_EQ(rejected, (std::vector<std::string> { "direct:process", "pointer:process", "spelled:process", "invoked:process", "second:fallback", "param:make",
                        "param_spelled:make", "local:launch", "run:member" }))
    << describe(scan.violations);
  EXPECT_EQ(scan.seen.at("process"), 5);
  EXPECT_EQ(scan.seen.at("make"), 2);
}

// Bodies the parser cannot recognise must fail closed instead of being mistaken for declarations.
TEST(DisplayRawMutationGateScanner, UnrecognisedFunctionBodiesFailClosed) {
  const auto scan = scan_sources({ { "a.cpp", R"(
    Foo::Foo(): member_ { 0 } {
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void tried() try {
      DisplayConfigSetDeviceInfo(nullptr);
    } catch (...) {}
    template <class T>
    void constrained() requires C<T> {
      DeviceIoControl(h, 0, nullptr, 0, nullptr, 0, nullptr, nullptr);
    })" } },
    {});
  ASSERT_EQ(scan.violations.size(), 3u) << describe(scan.violations);
  for (const auto &v : scan.violations) {
    EXPECT_EQ(v.why, "outside a recognised function body") << describe(scan.violations);
  }
}

TEST(DisplayRawMutationGateScanner, CapabilityParameterTypeIsMatchedAsAWholeParameterType) {
  const auto scan = scan_sources({ { "a.cpp", R"(
    void fake(const fake_mutation_capability_t &cap) {
      cap.require(1);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    topology::mutation_capability_t returned(const other_t &cap) {
      cap.require(1);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    }
    void second(int x, const topology::mutation_capability_t &cap) {
      cap.require(topology::operation_e::rotate);
      SetDisplayConfig(0, nullptr, 0, nullptr, 0);
    })" } },
    {});
  ASSERT_EQ(scan.violations.size(), 2u) << describe(scan.violations);
  EXPECT_EQ(scan.violations[0].function, "fake");
  EXPECT_EQ(scan.violations[1].function, "returned");
}

TEST(DisplayRawMutationGateScanner, PrimitivesMayOnlyBeCalledDirectlyAndNotFromMacros) {
  const auto scan = scan_sources({ { "a.cpp", R"(
    #define SNEAKY(x) \
      SetDisplayConfig(x, nullptr, 0, nullptr, 0)
    raw_fn select(const topology::mutation_capability_t &cap) {
      cap.require(topology::operation_e::rotate);
      return &SetDisplayConfig;
    })" } },
    {});
  ASSERT_EQ(scan.violations.size(), 2u) << describe(scan.violations);
  EXPECT_EQ(scan.violations[0].why, "inside a preprocessor directive");
  EXPECT_EQ(scan.violations[1].why, "not a direct call");

  // A gated pass to a synchronous seam is accepted only through an explicit indirect allowance, and never ungated.
  const auto passed = scan_sources({ { "src/a.cpp", R"(
    bool gated(const topology::mutation_capability_t &cap) {
      cap.require(topology::operation_e::write_hdr);
      return api::set(1, DisplayConfigSetDeviceInfo);
    }
    bool ungated(const topology::mutation_capability_t &cap) {
      return api::set(1, DisplayConfigSetDeviceInfo);
    })" } },
    { { "a.cpp", "gated", "DisplayConfigSetDeviceInfo", { "api::set(" }, {}, "test", true },
      { "a.cpp", "ungated", "DisplayConfigSetDeviceInfo", { "api::set(" }, {}, "test", true } });
  ASSERT_EQ(passed.violations.size(), 1u) << describe(passed.violations);
  EXPECT_EQ(passed.violations[0].function, "ungated");
  EXPECT_EQ(passed.violations[0].why, "not a direct call");
}

TEST(DisplayRawMutationGateScanner, AllowancesAreBoundToTheirJustifyingTokens) {
  const std::vector<allowance_t> allowances {
    { "a.cpp", "probe", "SetDisplayConfig", { "SDC_VALIDATE" }, { "SDC_APPLY" }, "test" },
  };
  const auto ok = scan_sources({ { "src/a.cpp", "bool probe() { return SetDisplayConfig(0, 0, 0, 0, SDC_VALIDATE); }" } }, allowances);
  EXPECT_TRUE(ok.violations.empty()) << describe(ok.violations);
  EXPECT_EQ(ok.used_allowances.size(), 1u);
  const auto applies = scan_sources({ { "src/a.cpp", "bool probe() { return SetDisplayConfig(0, 0, 0, 0, SDC_VALIDATE | SDC_APPLY); }" } }, allowances);
  ASSERT_EQ(applies.violations.size(), 1u);
  EXPECT_EQ(applies.violations[0].why, "allowance forbids SDC_APPLY");
  const auto renamed = scan_sources({ { "src/a.cpp", "bool probe2() { return SetDisplayConfig(0, 0, 0, 0, SDC_VALIDATE); }" } }, allowances);
  EXPECT_EQ(renamed.violations.size(), 1u);
  EXPECT_TRUE(renamed.used_allowances.empty());
  // An allowance justifies exactly one occurrence; a second write in the same function is not covered.
  const auto twice =
    scan_sources({ { "src/a.cpp", "bool probe() { SetDisplayConfig(0, 0, 0, 0, SDC_VALIDATE);\n return SetDisplayConfig(0, 0, 0, 0, 0); }" } }, allowances);
  ASSERT_EQ(twice.violations.size(), 1u) << describe(twice.violations);
  EXPECT_EQ(twice.violations[0].line, 2u);
  EXPECT_EQ(twice.violations[0].why, "allowance already used at line 1");
  // Allowances belong to the named function that owns a lambda.
  const std::vector<allowance_t> owner {
    { "a.cpp", "make_plan", "make_owned_actor_process", { "plan.process" }, {}, "test" },
  };
  const auto lambda = scan_sources({ { "src/a.cpp", "plan_t make_plan() { plan_t plan; plan.process = [] { return make_owned_actor_process({}); }; return plan; }" } }, owner);
  EXPECT_TRUE(lambda.violations.empty()) << describe(lambda.violations);
  EXPECT_EQ(lambda.used_allowances.size(), 1u);
}

TEST(DisplayRawMutationGate, EveryRawDisplayAndVddWriteInSourcesIsCapabilityGated) {
  const auto root = std::filesystem::path(SUNSHINE_SOURCE_DIR) / "src";
  ASSERT_TRUE(std::filesystem::is_directory(root)) << root;
  std::map<std::string, std::string> sources;
  static const std::set<std::string> extensions { ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".ixx" };
  for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || !extensions.contains(entry.path().extension().string())) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    ASSERT_TRUE(in) << entry.path();
    std::stringstream text;
    text << in.rdbuf();
    ASSERT_FALSE(in.bad()) << entry.path();
    sources.emplace(std::filesystem::relative(entry.path(), root).generic_string(), text.str());
  }
  const auto scan = scan_sources(sources, production_allowances);
  EXPECT_TRUE(scan.violations.empty()) << describe(scan.violations);

  for (std::size_t a = 0; a < production_allowances.size(); ++a) {
    EXPECT_TRUE(scan.used_allowances.contains(a)) << "stale allowance: " << production_allowances[a].file << ' ' << production_allowances[a].function
                                                  << ' ' << production_allowances[a].primitive;
  }
  // The scanner must still see the known writers; a silent miss would make the gate vacuous.
  for (const auto *expected : { "SetDisplayConfig", "ChangeDisplaySettingsExW", "DisplayConfigSetDeviceInfo", "DeviceIoControl", "IOCTL_VDD_COMMAND",
         "make_owned_actor_process", "adapter_restart_command", "make_process_actor_body", "make" }) {
    const auto found = scan.seen.find(expected);
    EXPECT_TRUE(found != scan.seen.end() && found->second > 0) << expected;
  }
}
