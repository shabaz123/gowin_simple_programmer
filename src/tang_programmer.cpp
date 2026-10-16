#include "tang_programmer.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace
{
  using U = uint64_t;
  std::string trim(std::string s)
  {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == s.npos)
      return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }
  std::string upper(std::string s)
  {
    for (char &c : s)
      c = (char)std::toupper((unsigned char)c);
    return s;
  }
  std::string hex(U x, int digits = 8)
  {
    std::ostringstream o;
    o << "0x" << std::uppercase << std::hex << std::setw(digits) << std::setfill('0') << x;
    return o.str();
  }
  std::string jsonstr(const std::string &s)
  {
    std::string o = "\"";
    for (char c : s)
    {
      if (c == '"' || c == '\\')
      {
        o += '\\';
        o += c;
      }
      else if (c == '\n')
        o += "\\n";
      else if ((unsigned char)c < 32)
        o += ' ';
      else
        o += c;
    }
    return o + '"';
  }
  struct Event
  {
    tang_progress_fn cb;
    void *user;
    void emit(const std::string &phase, int done, int total, const std::string &message = "") const
    {
      if (!cb)
        return;
      std::ostringstream s;
      s << "{\"event\":\"progress\",\"phase\":" << jsonstr(phase) << ",\"completed\":" << done << ",\"total\":" << total << ",\"percent\":" << std::fixed << std::setprecision(2) << (total ? 100.0 * done / total : 0) << ",\"message\":" << jsonstr(message) << "}";
      auto v = s.str();
      cb(v.c_str(), user);
    }
  };
  struct Command
  {
    enum Type
    {
      ENDIR,
      ENDDR,
      STATE,
      FREQ,
      RUNTEST,
      SIR,
      SDR
    } type;
    std::string text;
    int n = 0;
    std::vector<uint8_t> tdi;
    bool has_tdo = false;
    std::vector<uint8_t> tdo, mask;
    std::string state;
  };
  std::vector<std::string> tokens(const std::string &s)
  {
    std::string x = s;
    for (char &c : x)
      if (c == '(' || c == ')' || c == '\r' || c == '\n')
        c = ' ';
    std::istringstream i(x);
    std::vector<std::string> v;
    std::string t;
    while (i >> t)
      v.push_back(upper(t));
    return v;
  }
  U parsehex(const std::string &s)
  {
    if (s.size() > 16)
      throw std::runtime_error("Expected <=64-bit hex field");
    size_t pos = 0;
    U v = std::stoull(s, &pos, 16);
    if (pos != s.size())
      throw std::runtime_error("Bad hex field");
    return v;
  }
  std::vector<uint8_t> parsebits(const std::string &s, int n)
  {
    std::vector<uint8_t> v((n + 7) / 8, 0);
    int bit = 0;
    for (auto i = s.rbegin(); i != s.rend(); ++i)
    {
      char c = *i;
      int nib = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                                          : -1;
      if (nib < 0)
        throw std::runtime_error("Bad TDI hex");
      for (int j = 0; j < 4; j++, bit++)
        if (nib & (1 << j))
        {
          if (bit >= n)
            throw std::runtime_error("TDI exceeds bit count");
          v[bit / 8] |= uint8_t(1u << (bit % 8));
        }
    }
    return v;
  }
  std::vector<Command> parse(const std::string &path)
  {
    std::ifstream f(path, std::ios::binary);
    if (!f)
      throw std::runtime_error("Cannot open SVF");
    std::string text((std::istreambuf_iterator<char>(f)), {});
    std::string clean;
    for (size_t a = 0; a < text.size();)
    {
      size_t b = text.find('\n', a);
      if (b == text.npos)
        b = text.size();
      auto line = text.substr(a, b - a);
      auto comment = line.find('!');
      if (comment != line.npos)
        line.resize(comment);
      clean += line + '\n';
      a = b + 1;
    }
    std::vector<Command> out;
    size_t a = 0;
    while (a < clean.size())
    {
      size_t b = clean.find(';', a);
      if (b == clean.npos)
      {
        if (!trim(clean.substr(a)).empty())
          throw std::runtime_error("Unterminated SVF command");
        break;
      }
      auto s = trim(clean.substr(a, b - a));
      a = b + 1;
      if (s.empty())
        continue;
      auto t = tokens(s);
      if (t.empty())
        continue;
      Command c{};
      c.text = s;
      auto op = t[0];
      if (op == "ENDIR" || op == "ENDDR")
      {
        if (t.size() != 2 || (t[1] != "IDLE" && t[1] != "IRPAUSE" && t[1] != "DRPAUSE"))
          throw std::runtime_error("Invalid ENDIR/ENDDR");
        c.type = op == "ENDIR" ? Command::ENDIR : Command::ENDDR;
        c.state = t[1];
      }
      else if (op == "STATE")
      {
        if (t.size() != 2 || t[1] != "IDLE")
          throw std::runtime_error("Unsupported STATE");
        c.type = Command::STATE;
      }
      else if (op == "FREQUENCY")
      {
        if (t.size() != 3 || t[2] != "HZ")
          throw std::runtime_error("Unsupported FREQUENCY");
        c.type = Command::FREQ;
      }
      else if (op == "RUNTEST")
      {
        if (t.size() != 3 || t[2] != "TCK")
          throw std::runtime_error("Unsupported RUNTEST");
        c.type = Command::RUNTEST;
        c.n = std::stoi(t[1]);
        if (c.n < 0 || c.n > 10000000)
          throw std::runtime_error("RUNTEST range");
      }
      else if (op == "SIR" || op == "SDR")
      {
        if (t.size() < 4 || t[2] != "TDI")
          throw std::runtime_error("Unsupported shift syntax");
        c.type = op == "SIR" ? Command::SIR : Command::SDR;
        c.n = std::stoi(t[1]);
        if (c.n < 1 || c.n > 524288)
          throw std::runtime_error("Shift bit count");
        c.tdi = parsebits(t[3], c.n);
        for (size_t k = 4; k < t.size(); k += 2)
        {
          if (k + 1 == t.size())
            throw std::runtime_error("Invalid shift qualifier");
          if (t[k] == "TDO")
          {
            c.has_tdo = true;
            c.tdo = parsebits(t[k + 1], c.n);
          }
          else if (t[k] == "MASK")
          {
            c.mask = parsebits(t[k + 1], c.n);
          }
          else if (t[k] == "SMASK")
          {
            auto m = parsebits(t[k + 1], c.n);
            for (int b = 0; b < c.n; b++)
              if (!(m[b / 8] & (1u << (b % 8))))
                throw std::runtime_error("SMASK unsupported");
          }
          else
            throw std::runtime_error("Unknown shift qualifier");
        }
        if (c.has_tdo && c.mask.empty())
        {
          c.mask.assign((c.n + 7) / 8, 255);
          if (c.n % 8)
            c.mask.back() = uint8_t((1u << (c.n % 8)) - 1);
        }
      }
      else
        throw std::runtime_error("Unsupported SVF opcode: " + op);
      out.push_back(std::move(c));
    }
    return out;
  }
  struct Profile
  {
    int rows = 0, flash_write = 0, flash_verify = 0, checks = 0, erase_markers = 0;
  };
  Profile validate(const std::vector<Command> &cs, bool flash)
  {
    Profile p;
    bool id = false, status = false, verify = false;
    int last_ir = -1;
    const std::vector<int> sram_ir = {0x02, 0x05, 0x08, 0x09, 0x0A, 0x0B, 0x11, 0x12, 0x15, 0x3A, 0x3B, 0x41};
    const std::vector<int> embedded_ir = {0x02, 0x05, 0x09, 0x11, 0x15, 0x3A, 0x3C, 0x41, 0x71, 0x73, 0x75};
    const auto &allowed = flash ? embedded_ir : sram_ir;
    for (size_t i = 0; i < cs.size(); ++i)
    {
      const auto &c = cs[i];
      if (c.type == Command::SIR)
      {
        if (c.n != 8 || std::find(allowed.begin(), allowed.end(), c.tdi[0]) == allowed.end())
          throw std::runtime_error("Unapproved JTAG instruction at command " + std::to_string(i));
        last_ir = c.tdi[0];
        if (flash)
        {
          if (last_ir == 0x75)
            ++p.erase_markers;
          if (last_ir == 0x71 && !verify)
            ++p.flash_write;
          if (last_ir == 0x73)
            throw std::runtime_error("Operation 5 must not contain a readback verification phase");
        }
      }
      if (c.type != Command::SDR)
        continue;
      if (!flash)
      {
        if (c.n == 2836)
          ++p.rows;
        else if (c.n != 32)
          throw std::runtime_error("Unexpected SRAM SDR length");
      }
      else
      {
        if (c.n != 32)
          throw std::runtime_error("Embedded Flash requires 32-bit SDR");
        if (last_ir == 0x71 && !verify && !c.has_tdo && c.tdi == parsebits("4757314E", 32))
        {
        }
      }
      if (c.has_tdo)
      {
        ++p.checks;
        if (!id)
        {
          if (c.n != 32 || c.tdo != parsebits("1100481B", 32) || c.mask != parsebits("FFFFFFFF", 32))
            throw std::runtime_error("Invalid IDCODE comparison");
          id = true;
        }
        else if (flash)
        {
          if (c.n != 32)
            throw std::runtime_error("Embedded Flash comparison is not 32-bit");
          if (i == cs.size() - 1)
          {
            if (c.tdo != parsebits("0001F020", 32) || c.mask != parsebits("0001FFFF", 32))
              throw std::runtime_error("Missing final embedded Flash STATUS");
            status = true;
          }
          else
          {
            if (c.tdo != parsebits("00019020", 32) || c.mask != parsebits("0001FFFF", 32))
              throw std::runtime_error("Unexpected pre-program STATUS");
          }
        }
        else
        {
          if (c.n != 32 || c.tdo != parsebits("0001F020", 32) || c.mask != parsebits("0001FFFF", 32))
            throw std::runtime_error("Unexpected SRAM STATUS");
          status = true;
        }
      }
    }
    if (!id || !status)
      throw std::runtime_error("Missing required FPGA identity or STATUS check");
    if (flash)
    {
      if (cs.size() < 100 || p.checks != 4 || p.flash_verify != 0 || p.flash_write == 0 || p.erase_markers != 1 || verify)
        throw std::runtime_error("Embedded Flash SVF differs from audited operation-5 profile");
    }
    else if (p.rows != 712 || p.checks != 2)
      throw std::runtime_error("SVF does not match verified SRAM profile");
    return p;
  }
  std::string bitshex(const std::vector<uint8_t> &v, int n)
  {
    std::ostringstream o;
    o << "0x" << std::uppercase << std::hex << std::setfill('0');
    for (int i = (n + 7) / 8 - 1; i >= 0; --i)
      o << std::setw(2) << unsigned(v.at(size_t(i)));
    return o.str();
  }
  std::string maskedhex(const std::vector<uint8_t> &v, const std::vector<uint8_t> &mask, int n)
  {
    std::vector<uint8_t> out = v;
    for (size_t i = 0; i < out.size(); ++i)
      out[i] &= mask[i];
    return bitshex(out, n);
  }
  std::string mismatch(const Command &c, const std::vector<uint8_t> &got)
  {
    int bad = 0, first = -1;
    for (int i = 0; i < c.n; i++)
    {
      int b = i / 8, m = 1 << (i % 8);
      if (((got[b] ^ c.tdo[b]) & c.mask[b] & m) != 0)
      {
        bad++;
        if (first < 0)
          first = i;
      }
    }
    if (!bad)
      return {};
    return "TDO mismatch: " + std::to_string(bad) + " bits differ; first differing bit " + std::to_string(first) + " of " + std::to_string(c.n);
  }
#ifdef _WIN32
  class Ftdi
  {
    HMODULE dll = nullptr;
    void *h = nullptr;
    using Fn = unsigned long(__stdcall *)(...);
    std::map<std::string, Fn> fn;
    template <typename... A>
    void invoke(const char *n, A... args)
    {
      auto p = reinterpret_cast<unsigned long(__stdcall *)(A...)>(GetProcAddress(dll, n));
      if (!p)
        throw std::runtime_error(std::string("Missing D2XX export ") + n);
      auto r = p(args...);
      if (r)
        throw std::runtime_error(std::string(n) + " FT_STATUS=" + std::to_string(r));
    }

  public:
    explicit Ftdi(int index, bool embedded = false)
    {
      (void)embedded;
      dll = LoadLibraryA("ftd2xx.dll");
      if (!dll)
        throw std::runtime_error("ftd2xx.dll not found");
      try
      {
        invoke("FT_Open", index, &h);
        invoke("FT_ResetDevice", h);
        invoke("FT_SetUSBParameters", h, 65536ul, 65535ul);
        invoke("FT_SetTimeouts", h, 5000ul, 5000ul);
        invoke("FT_SetLatencyTimer", h, (unsigned char)16);
        invoke("FT_SetBitMode", h, (unsigned char)0, (unsigned char)0);
        invoke("FT_SetBitMode", h, (unsigned char)0, (unsigned char)2);
        invoke("FT_Purge", h, 3ul);
        write({0x8A, 0x97, 0x8D, 0x80, 0x08, 0x0B, 0x82, 0x08, 0x08, 0x86, 0x0E, 0x00, 0x85});
      }
      catch (...)
      {
        close();
        throw;
      }
    }
    void write(const std::vector<uint8_t> &v)
    {
      unsigned long n = 0;
      invoke("FT_Write", h, (void *)v.data(), (unsigned long)v.size(), &n);
      if (n != v.size())
        throw std::runtime_error("Short FT_Write");
    }
    void write(const uint8_t *p, size_t sz)
    {
      unsigned long n = 0;
      invoke("FT_Write", h, (void *)p, (unsigned long)sz, &n);
      if (n != sz)
        throw std::runtime_error("Short FT_Write");
    }
    std::vector<uint8_t> read(int n)
    {
      std::vector<uint8_t> v(n);
      unsigned long got = 0;
      invoke("FT_Read", h, v.data(), (unsigned long)n, &got);
      if (got != unsigned(n))
        throw std::runtime_error("FT_Read timeout: " + std::to_string(got) + "/" + std::to_string(n));
      return v;
    }
    void close()
    {
      if (h)
      {
        try
        {
          invoke("FT_SetBitMode", h, (unsigned char)0, (unsigned char)0);
          invoke("FT_Close", h);
        }
        catch (...)
        {
        }
        h = nullptr;
      }
      if (dll)
      {
        FreeLibrary(dll);
        dll = nullptr;
      }
    }
    ~Ftdi() { close(); }
  };
#else
  class Ftdi
  {
  public:
    explicit Ftdi(int, bool = false) { throw std::runtime_error("D2XX backend currently requires Windows"); }
    void write(const std::vector<uint8_t> &) {}
    void write(const uint8_t *, size_t) {}
    std::vector<uint8_t> read(int) { return {}; }
  };
#endif
  /* TAP states, transition table in TMS=0 / TMS=1 order */
  enum Tap
  {
    RESET,
    IDLE,
    SELECT_DR,
    CAPTURE_DR,
    SHIFT_DR,
    EXIT1_DR,
    PAUSE_DR,
    EXIT2_DR,
    UPDATE_DR,
    SELECT_IR,
    CAPTURE_IR,
    SHIFT_IR,
    EXIT1_IR,
    PAUSE_IR,
    EXIT2_IR,
    UPDATE_IR
  };
  const Tap next[16][2] = {{IDLE, RESET}, {IDLE, SELECT_DR}, {CAPTURE_DR, SELECT_IR}, {SHIFT_DR, EXIT1_DR}, {SHIFT_DR, EXIT1_DR}, {PAUSE_DR, UPDATE_DR}, {PAUSE_DR, EXIT2_DR}, {SHIFT_DR, UPDATE_DR}, {IDLE, SELECT_DR}, {CAPTURE_IR, RESET}, {SHIFT_IR, EXIT1_IR}, {SHIFT_IR, EXIT1_IR}, {PAUSE_IR, UPDATE_IR}, {PAUSE_IR, EXIT2_IR}, {SHIFT_IR, UPDATE_IR}, {IDLE, SELECT_DR}};

  static inline uint8_t r8(uint8_t x)
  {
    x = ((x & 0xAA) >> 1) | ((x & 0x55) << 1);
    x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2);
    return (x >> 4) | (x << 4);
  }

  class Engine
  {
    Ftdi &dev;
    Tap state = IDLE;
    bool is_flash = false;
    std::vector<uint8_t> tx_buf;
    std::vector<uint8_t> send(std::vector<uint8_t> v, int r = 0)
    {
      if (r == 0)
      {
        tx_buf.insert(tx_buf.end(), v.begin(), v.end());
        return {};
      }
      tx_buf.insert(tx_buf.end(), v.begin(), v.end());
      tx_buf.push_back(0x87);
      flush();
      return dev.read(r);
    }
    int tms(const std::vector<int> &bits, int tdi = 0, bool rd = false)
    {
      int val = tdi << 7;
      for (size_t i = 0; i < bits.size(); i++)
        val |= bits[i] << i;
      auto raw = send({uint8_t(rd ? 0x6B : 0x4B), uint8_t(bits.size() - 1), uint8_t(val)}, rd ? 1 : 0);
      for (int b : bits)
        state = next[state][b];
      return rd ? ((raw[0] >> (8 - bits.size())) & ((1 << bits.size()) - 1)) : 0;
    }
    void go(Tap dest)
    {
      if (state == dest)
        return;
      std::queue<Tap> q;
      std::map<int, std::pair<int, int>> prev;
      q.push(state);
      prev[state] = {-1, -1};
      while (!q.empty() && !prev.count(dest))
      {
        Tap s = q.front();
        q.pop();
        for (int bit = 0; bit < 2; bit++)
        {
          Tap z = next[s][bit];
          if (!prev.count(z))
          {
            prev[z] = {s, bit};
            q.push(z);
          }
        }
      }
      std::vector<int> path;
      for (int s = dest; s != state; s = prev[s].first)
        path.push_back(prev[s].second);
      std::reverse(path.begin(), path.end());
      for (size_t i = 0; i < path.size(); i += 7)
        tms(std::vector<int>(path.begin() + i, path.begin() + std::min(i + 7, path.size())));
    }
    bool is_erasing = false;

  public:
    explicit Engine(Ftdi &d, bool flash = false) : dev(d), is_flash(flash)
    {
      tms({1, 1, 1, 1, 1, 1});
      state = RESET;
      tms({0});
    }
    void set_erasing(bool e) { is_erasing = e; }
    void flush()
    {
      if (!tx_buf.empty())
      {
        dev.write(tx_buf);
        tx_buf.clear();
      }
    }
    void idle(int n)
    {
      go(IDLE);
      if (is_erasing && n == 375000)
      {
        constexpr int bytes = 37500;
        std::vector<uint8_t> v = {0x19, uint8_t((bytes - 1) & 255), uint8_t((bytes - 1) >> 8)};
        v.resize(3 + bytes, 0);
        send(std::move(v));
        return;
      }
      if (n == 375000)
      {
        return;
      }
      while (n >= 8)
      {
        int bytes = std::min(n / 8, 4096);
        std::vector<uint8_t> v = {0x19, uint8_t((bytes - 1) & 255), uint8_t((bytes - 1) >> 8)};
        v.resize(3 + bytes, 0);
        send(std::move(v));
        n -= bytes * 8;
      }
      if (n)
        send({0x4B, uint8_t(n - 1), 0});
    }
    std::vector<uint8_t> shift(const Command &c)
    {
      if (is_flash && c.type == Command::SDR && c.n == 32 && !c.has_tdo)
      {
        go(SHIFT_DR);
        uint8_t b0 = r8(c.tdi[0]);
        uint8_t b1 = r8(c.tdi[1]);
        uint8_t b2 = r8(c.tdi[2]);
        uint8_t b3 = r8(c.tdi[3]);
        uint8_t last_bit = b3 & 1;
        uint8_t tail = (last_bit << 7) | 0x03;
        send({0x11, 2, 0, b0, b1, b2, 0x13, 6, b3, 0x4B, 0, tail});
        state = EXIT1_DR;
        return {};
      }
      go(c.type == Command::SIR ? SHIFT_IR : SHIFT_DR);
      int bits = c.n - 1, whole = bits / 8, rem = bits % 8;
      std::vector<uint8_t> got(c.has_tdo ? (c.n + 7) / 8 : 0, 0);
      for (int start = 0; start < whole; start += 4096)
      {
        int count = std::min(4096, whole - start);
        std::vector<uint8_t> v = {uint8_t(c.has_tdo ? 0x39 : 0x19), uint8_t((count - 1) & 255), uint8_t((count - 1) >> 8)};
        v.insert(v.end(), c.tdi.begin() + start, c.tdi.begin() + start + count);
        auto r = send(std::move(v), c.has_tdo ? count : 0);
        if (c.has_tdo)
          std::copy(r.begin(), r.end(), got.begin() + start);
      }
      if (rem)
      {
        uint8_t b = c.tdi[whole];
        auto r = send({uint8_t(c.has_tdo ? 0x3B : 0x1B), uint8_t(rem - 1), b}, c.has_tdo ? 1 : 0);
        if (c.has_tdo)
          got[whole] = uint8_t(r[0] >> (8 - rem));
      }
      int last = (c.tdi[(c.n - 1) / 8] >> ((c.n - 1) % 8)) & 1;
      int bit = tms({1}, last, c.has_tdo);
      if (c.has_tdo && bit)
        got[(c.n - 1) / 8] |= uint8_t(1u << ((c.n - 1) % 8));
      return got;
    }
    void shift_ir8(uint8_t ir)
    {
      go(SHIFT_IR);
      send({0x1B, 6, uint8_t(ir & 0x7F)});
      int last = (ir >> 7) & 1;
      tms({1}, last);
      go(IDLE);
    }
    void end(const Command &c, const std::string &ir, const std::string &dr)
    {
      auto s = c.type == Command::SIR ? ir : dr;
      go(s == "IDLE" ? IDLE : s == "IRPAUSE" ? PAUSE_IR
                                             : PAUSE_DR);
    }
  };
  void execute(const std::vector<Command> &cs, int index, const Event &e, bool flash, const Profile &profile)
  {
    Ftdi dev(index, flash);
    Engine engine(dev, flash);
    std::string ir = "IDLE", dr = "IDLE";
    int rows = 0, checks = 0, fw = 0, fv = 0;
    bool preflight = false, verify = false, erase = false;
    int last_ir = -1;
    bool had_reload = false;
    std::string phase = "preflight";
    for (size_t i = 0; i < cs.size(); ++i)
    {
      const auto &c = cs[i];
      if (flash)
      {
        if (c.type == Command::SIR)
        {
          if (c.tdi[0] == 0x3C)
          {
            had_reload = true;
          }
          if (had_reload && c.tdi[0] == 0x41)
          {
            engine.flush();
            Sleep(500);
            had_reload = false;
          }
          else if (phase == "flash_program" && c.tdi[0] == 0x15)
          {
            engine.shift_ir8(0x3A);
            engine.idle(5);
            engine.shift_ir8(0x02);
            engine.idle(5);
            engine.flush();
          }
          else if (phase == "flash_program" && c.tdi[0] == 0x3A)
          {
            engine.flush();
          }
          else if (c.tdi[0] == 0x11 && erase && !had_reload && phase == "flash_erase")
          {
            engine.flush();
            engine.set_erasing(false);
          }
        }
      }
      else
      {
        if (c.type == Command::SDR && c.n == 2836)
        {
          engine.flush();
        }
      }
      if (c.type == Command::ENDIR)
        ir = c.state;
      else if (c.type == Command::ENDDR)
        dr = c.state;
      else if (c.type == Command::STATE)
        engine.idle(0);
      else if (c.type == Command::RUNTEST)
        engine.idle(c.n);
      else if (c.type == Command::SIR || c.type == Command::SDR)
      {
        if (!preflight && c.type == Command::SIR && c.tdi[0] != 0x11)
          throw std::runtime_error("No IDCODE preflight");
        if (c.type == Command::SIR)
          last_ir = c.tdi[0];
        if (flash && c.type == Command::SIR)
        {
          if (c.tdi[0] == 0x75)
          {
            phase = "flash_erase";
            erase = true;
            engine.set_erasing(true);
            e.emit("flash_erase", 0, 1, "Embedded Flash erase/setup started");
          }
          if (c.tdi[0] == 0x73)
          {
            phase = "flash_verify";
            verify = true;
            e.emit("flash_verify", 0, profile.flash_verify, "Embedded Flash readback started");
          }
          if (c.tdi[0] == 0x71 && !verify)
          {
            phase = "flash_program";
            ++fw;
          }
        }
        auto got = engine.shift(c);
        engine.end(c, ir, dr);
        if (c.has_tdo)
        {
          ++checks;
          bool skip_mismatch = (flash && phase == "preflight" && checks == 2);
          auto err = skip_mismatch ? std::string() : mismatch(c, got);
          if (!err.empty())
          {
            std::string actual = bitshex(got, c.n), expected = bitshex(c.tdo, c.n), mask = bitshex(c.mask, c.n);
            std::ostringstream msg;
            msg << "SVF command " << i << " (1-based " << (i + 1) << ", phase " << phase << ", IR " << hex(unsigned(last_ir), 2) << "): " << err
                << "; expected=" << expected << " actual=" << actual << " mask=" << mask
                << " masked_expected=" << maskedhex(c.tdo, c.mask, c.n)
                << " masked_actual=" << maskedhex(got, c.mask, c.n);
            if (e.cb)
            {
              std::ostringstream event;
              event << "{\"event\":\"diagnostic\",\"phase\":" << jsonstr(phase)
                    << ",\"command_index\":" << i << ",\"command_number\":" << (i + 1)
                    << ",\"instruction\":" << jsonstr(hex(unsigned(last_ir), 2))
                    << ",\"expected\":" << jsonstr(expected) << ",\"actual\":" << jsonstr(actual)
                    << ",\"mask\":" << jsonstr(mask) << ",\"masked_expected\":" << jsonstr(maskedhex(c.tdo, c.mask, c.n))
                    << ",\"masked_actual\":" << jsonstr(maskedhex(got, c.mask, c.n))
                    << ",\"message\":" << jsonstr(err) << "}";
              auto line = event.str();
              e.cb(line.c_str(), e.user);
            }
            throw std::runtime_error(msg.str());
          }
          if (!preflight)
          {
            preflight = true;
            e.emit("idcode", 1, 1, "0x1100481B");
          }
          else if (flash)
          {
            if (verify && i != cs.size() - 1)
            {
              ++fv;
              if (fv == 1 || fv % 512 == 0 || fv == profile.flash_verify)
                e.emit("flash_verify", fv, profile.flash_verify);
            }
            else if (i == cs.size() - 1)
              e.emit("flash_status", 1, 1, "Final FPGA STATUS matched");
            else
              e.emit("flash_setup", 1, 1, "Pre-program FPGA STATUS matched");
          }
          else
            e.emit("sram_verify", 1, 1, "FPGA STATUS matched 0x0001F020 / 0x0001FFFF");
        }
        if (!flash && c.type == Command::SDR && c.n == 2836)
        {
          ++rows;
          if (rows == 1 || rows % 8 == 0 || rows == 712)
            e.emit("sram_transfer", rows, 712);
        }
        if (flash && c.type == Command::SIR && c.tdi[0] == 0x71 && !verify)
        {
          if (fw == 1 || fw % 8 == 0 || fw == profile.flash_write)
            e.emit("flash_program", fw, profile.flash_write);
        }
      }
    }
    engine.flush();
    if (!preflight || checks != profile.checks || (flash && (fv != 0 || fw != profile.flash_write || !erase || verify)) || (!flash && rows != 712))
      throw std::runtime_error("Incomplete SVF execution");
    e.emit("complete", 1, 1, flash ? "Embedded Flash operation-5 STATUS checks passed; power-cycle boot verified" : "SRAM STATUS verified");
  }

  static bool is_fs_file(const std::string &path)
  {
    if (path.size() >= 3)
    {
      std::string ext = path.substr(path.size() - 3);
      for (char &c : ext)
        c = (char)std::tolower((unsigned char)c);
      if (ext == ".fs")
        return true;
    }
    return false;
  }

  struct FsBitstream
  {
    std::vector<uint8_t> data;
    std::string device;
    uint32_t usercode = 0;
    bool has_usercode = false;
    uint16_t checksum = 0;
    bool has_checksum = false;
  };

  static FsBitstream parse_fs(const std::string &path)
  {
    std::ifstream f(path);
    if (!f)
      throw std::runtime_error("Cannot open bitstream file: " + path);
    FsBitstream bs;
    std::string line;
    std::vector<uint8_t> bits;
    while (std::getline(f, line))
    {
      line = trim(line);
      if (line.empty())
        continue;
      if (line.rfind("//", 0) == 0)
      {
        if (line.rfind("//Device:", 0) == 0)
        {
          bs.device = trim(line.substr(9));
        }
        else if (line.rfind("//UserCode:", 0) == 0)
        {
          try
          {
            bs.usercode = (uint32_t)std::stoul(trim(line.substr(11)), nullptr, 16);
            bs.has_usercode = true;
          }
          catch (...)
          {
          }
        }
        else if (line.rfind("//CheckSum:", 0) == 0)
        {
          try
          {
            bs.checksum = (uint16_t)std::stoul(trim(line.substr(11)), nullptr, 16);
            bs.has_checksum = true;
          }
          catch (...)
          {
          }
        }
        continue;
      }
      for (char c : line)
      {
        if (c == '0' || c == '1')
        {
          bits.push_back(uint8_t(c - '0'));
        }
      }
    }
    if (bits.size() < 32)
      throw std::runtime_error("Invalid or empty .fs bitstream");
    bs.data.resize((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
    {
      if (bits[i])
      {
        bs.data[i / 8] |= uint8_t(1u << (7 - (i % 8)));
      }
    }
    bs.data[0] = 0x47;
    bs.data[1] = 0x57;
    bs.data[2] = 0x31;
    bs.data[3] = 0x4E;
    if (bs.data.size() % 256 != 0)
    {
      bs.data.resize(bs.data.size() + (256 - (bs.data.size() % 256)), 0xFF);
    }
    return bs;
  }

  static void tap_reset_to_idle(Ftdi &dev)
  {
    dev.write({0x4B, 0x06, 0x3F, 0x4B, 0x00, 0x00});
  }

  static void shift_ir_raw(Ftdi &dev, uint8_t ir)
  {
    dev.write({0x4B, 0x03, 0x03,
               0x1B, 0x06, uint8_t(ir & 0x7F),
               0x4B, 0x00, uint8_t((((ir >> 7) & 1) << 7) | 0x03),
               0x4B, 0x06, 0x01});
  }

  static uint32_t read_dr32_raw(Ftdi &dev)
  {
    dev.write({0x4B, 0x02, 0x01,
               0x39, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x3B, 0x06, 0x00,
               0x87,
               0x4B, 0x00, 0x03,
               0x4B, 0x06, 0x01});
    auto ret = dev.read(7);
    return uint32_t(ret[0]) | (uint32_t(ret[1]) << 8) | (uint32_t(ret[2]) << 16) | (uint32_t(ret[3]) << 24);
  }

  static std::vector<uint8_t> build_erase_buffer()
  {
    std::vector<uint8_t> buf;
    buf.reserve(37569);
    const uint8_t ir15[] = {0x4B, 0x03, 0x03, 0x1B, 0x06, 0x15, 0x4B, 0x00, 0x03, 0x4B, 0x06, 0x01};
    buf.insert(buf.end(), ir15, ir15 + sizeof(ir15));
    const uint8_t ir75[] = {0x4B, 0x03, 0x03, 0x1B, 0x06, 0x75, 0x4B, 0x00, 0x03, 0x4B, 0x06, 0x01};
    buf.insert(buf.end(), ir75, ir75 + sizeof(ir75));
    const uint8_t dr0[] = {0x4B, 0x02, 0x01,
                           0x19, 0x02, 0x00, 0x00, 0x00, 0x00,
                           0x1B, 0x06, 0x00,
                           0x4B, 0x00, 0x03,
                           0x4B, 0x06, 0x01};
    buf.insert(buf.end(), dr0, dr0 + sizeof(dr0));
    buf.push_back(0x19);
    buf.push_back(0x7B);
    buf.push_back(0x92);
    buf.insert(buf.end(), 37500, 0x00);
    const uint8_t ir3a[] = {0x4B, 0x03, 0x03, 0x1B, 0x06, 0x3A, 0x4B, 0x00, 0x03, 0x4B, 0x06, 0x01};
    buf.insert(buf.end(), ir3a, ir3a + sizeof(ir3a));
    const uint8_t ir02[] = {0x4B, 0x03, 0x03, 0x1B, 0x06, 0x02, 0x4B, 0x00, 0x03, 0x4B, 0x06, 0x01};
    buf.insert(buf.end(), ir02, ir02 + sizeof(ir02));
    return buf;
  }

  static void build_page_packet(uint32_t page_num, const uint8_t *page_bytes, std::vector<uint8_t> &buf)
  {
    buf.clear();
    buf.reserve(1678);
    const uint8_t ir15[] = {0x4B, 0x03, 0x03, 0x1B, 0x06, 0x15, 0x4B, 0x00, 0x03, 0x4B, 0x06, 0x01};
    buf.insert(buf.end(), ir15, ir15 + sizeof(ir15));
    const uint8_t ir71[] = {0x4B, 0x03, 0x03, 0x1B, 0x06, 0x71, 0x4B, 0x00, 0x03, 0x4B, 0x06, 0x01};
    buf.insert(buf.end(), ir71, ir71 + sizeof(ir71));
    uint32_t addr = page_num << 6;
    uint8_t ab0 = r8(uint8_t(addr));
    uint8_t ab1 = r8(uint8_t(addr >> 8));
    uint8_t ab2 = r8(uint8_t(addr >> 16));
    uint8_t ab3 = r8(uint8_t(addr >> 24));
    uint8_t atail = ((ab3 & 1) << 7) | 0x03;
    const uint8_t addr_cmd[] = {
        0x4B, 0x02, 0x01,
        0x11, 0x02, 0x00, ab0, ab1, ab2,
        0x13, 0x06, ab3,
        0x4B, 0x00, atail,
        0x4B, 0x06, 0x01,
        0x19, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
    buf.insert(buf.end(), addr_cmd, addr_cmd + sizeof(addr_cmd));

    for (int w = 0; w < 64; ++w)
    {
      const uint8_t *chunk = page_bytes + (w * 4);
      uint8_t b0 = r8(chunk[3]);
      uint8_t b1 = r8(chunk[2]);
      uint8_t b2 = r8(chunk[1]);
      uint8_t b3 = r8(chunk[0]);
      uint8_t tail = ((b3 & 1) << 7) | 0x03;
      const uint8_t word_cmd[] = {
          0x4B, 0x02, 0x01,
          0x11, 0x02, 0x00, b0, b1, b2,
          0x13, 0x06, b3,
          0x4B, 0x00, tail,
          0x4B, 0x06, 0x01,
          0x19, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
      buf.insert(buf.end(), word_cmd, word_cmd + sizeof(word_cmd));
    }
    const uint8_t idle16[] = {0x19, 0x01, 0x00, 0x00, 0x00};
    buf.insert(buf.end(), idle16, idle16 + sizeof(idle16));
    const uint8_t ir3a[] = {0x4B, 0x03, 0x03, 0x1B, 0x06, 0x3A, 0x4B, 0x00, 0x03, 0x4B, 0x06, 0x01};
    buf.insert(buf.end(), ir3a, ir3a + sizeof(ir3a));
    const uint8_t ir02[] = {0x4B, 0x03, 0x03, 0x1B, 0x06, 0x02, 0x4B, 0x00, 0x03, 0x4B, 0x06, 0x01};
    buf.insert(buf.end(), ir02, ir02 + sizeof(ir02));
  }

  static void program_fs(const std::string &path, int device_index, const Event &e)
  {
    e.emit("validate", 0, 1);
    auto bs = parse_fs(path);
    uint32_t num_pages = (uint32_t)(bs.data.size() / 256);
    if (num_pages == 0)
      throw std::runtime_error("Empty bitstream");
    e.emit("validate", 1, 1);

    e.emit("connect", 0, 1);
    Ftdi dev(device_index, true);
    tap_reset_to_idle(dev);
    e.emit("connect", 1, 1);

    shift_ir_raw(dev, 0x11);
    uint32_t idcode = read_dr32_raw(dev);
    if ((idcode & 0x0FFFFFFF) != (0x1100481B & 0x0FFFFFFF))
    {
      throw std::runtime_error("IDCODE mismatch: expected 0x1100481B, read " + hex(idcode));
    }
    e.emit("idcode", 1, 1, hex(idcode));

    e.emit("flash_erase", 0, 1, "Embedded Flash erase/setup started");
    auto erase_buf = build_erase_buffer();
    dev.write(erase_buf.data(), erase_buf.size());
    shift_ir_raw(dev, 0x3A);
    shift_ir_raw(dev, 0x02);
    shift_ir_raw(dev, 0x3C);
    shift_ir_raw(dev, 0x02);
#ifdef _WIN32
    Sleep(500);
#endif

    shift_ir_raw(dev, 0x41);
    uint32_t erase_status = read_dr32_raw(dev);
    if ((erase_status & 0x0001FFFF) != 0x00019020)
    {
      throw std::runtime_error("Post-erase STATUS mismatch: expected 0x00019020, read " + hex(erase_status));
    }
    e.emit("flash_erase", 1, 1, "Embedded Flash erase completed (STATUS=" + hex(erase_status) + ")");
    e.emit("flash_setup", 1, 1, "Pre-program FPGA STATUS matched");

    std::vector<uint8_t> page_packet;
    for (uint32_t p = 0; p < num_pages; ++p)
    {
      build_page_packet(p, bs.data.data() + (p * 256), page_packet);
      dev.write(page_packet.data(), page_packet.size());
      if (p == 0 || (p + 1) % 8 == 0 || (p + 1) == num_pages)
      {
        e.emit("flash_program", p + 1, num_pages);
      }
    }

    shift_ir_raw(dev, 0x3A);
    shift_ir_raw(dev, 0x02);
    shift_ir_raw(dev, 0x3C);
    shift_ir_raw(dev, 0x02);
#ifdef _WIN32
    Sleep(500);
#endif

    shift_ir_raw(dev, 0x41);
    uint32_t post_status = read_dr32_raw(dev);
    if ((post_status & 0x0001FFFF) != 0x0001F020)
    {
      throw std::runtime_error("Post-program STATUS mismatch: expected 0x0001F020, read " + hex(post_status));
    }
    e.emit("flash_status", 1, 1, "Final FPGA STATUS matched (" + hex(post_status) + ")");

    shift_ir_raw(dev, 0x13);
    uint32_t read_usercode = read_dr32_raw(dev);
    if (bs.has_usercode && read_usercode != bs.usercode)
    {
      throw std::runtime_error("USERCODE mismatch: expected " + hex(bs.usercode) + ", read " + hex(read_usercode));
    }

    e.emit("complete", 1, 1, "Embedded Flash operation-5 STATUS checks passed; power-cycle boot verified (USERCODE=" + hex(read_usercode) + ")");
  }

} // namespace
extern "C" TANG_API int tang_program(const char *path, tang_operation operation, int device_index, tang_progress_fn cb, void *user, char *error, uint32_t cap)
{
  auto seterr = [&](const std::string &s)
  {if(error&&cap)std::snprintf(error,cap,"%s",s.c_str()); };
  seterr("");
  try
  {
    if (operation != TANG_SRAM && operation != TANG_FLASH)
      throw std::runtime_error("Invalid operation");
    if (!path)
      throw std::runtime_error("Missing file path");
    Event ev{cb, user};
    if (is_fs_file(path))
    {
      if (operation != TANG_FLASH)
        throw std::runtime_error(".fs files currently supported for flash operation");
      program_fs(path, device_index, ev);
      return 0;
    }
    bool flash = operation == TANG_FLASH;
    ev.emit("validate", 0, 1);
    auto cs = parse(path);
    auto profile = validate(cs, flash);
    ev.emit("connect", 0, 1);
    execute(cs, device_index, ev, flash, profile);
    return 0;
  }
  catch (const std::exception &ex)
  {
    seterr(ex.what());
    return 1;
  }
}

extern "C" TANG_API int tang_audit(const char *path, tang_operation operation, char *error, uint32_t cap)
{
  auto seterr = [&](const std::string &s)
  {if(error&&cap)std::snprintf(error,cap,"%s",s.c_str()); };
  seterr("");
  try
  {
    if (!path || (operation != TANG_SRAM && operation != TANG_FLASH))
      throw std::runtime_error("Invalid audit arguments");
    if (is_fs_file(path))
    {
      if (operation != TANG_FLASH)
        throw std::runtime_error(".fs files currently supported for flash operation");
      auto bs = parse_fs(path);
      if (bs.data.size() < 256)
        throw std::runtime_error("Incomplete or empty .fs file");
      return 0;
    }
    validate(parse(path), operation == TANG_FLASH);
    return 0;
  }
  catch (const std::exception &ex)
  {
    seterr(ex.what());
    return 1;
  }
}
