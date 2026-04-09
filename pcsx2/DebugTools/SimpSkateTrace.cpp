// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/SimpSkateTrace.h"

#include "Memory.h"
#include "R5900.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace SimpSkateTrace
{
namespace
{
struct PcRange
{
	u32 start = 0;
	u32 end = 0;
};

static std::mutex s_lock;
static bool s_init = false;
static bool s_enabled = false;
static std::FILE* s_fp = nullptr;
static std::array<u32, 32> s_tracepoints = {};
static size_t s_tracepoint_count = 0;
static std::array<PcRange, 32> s_trace_ranges = {};
static size_t s_trace_range_count = 0;
static bool s_log_ptr_windows = true;
static bool s_log_parser_snapshots = true;
static bool s_log_object_candidates = true;
static u32 s_ptr_window_bytes = 96;
static u32 s_stack_words = 32;
static u64 s_max_events = 0;
static u64 s_seq = 0;
static bool s_limit_reached = false;

static std::ostringstream NewJsonStream()
{
	std::ostringstream os;
	os.imbue(std::locale::classic());
	return os;
}

static std::string Trim(std::string s)
{
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
	s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), s.end());
	return s;
}

static std::string JsonEscape(const std::string_view& text)
{
	std::string out;
	out.reserve(text.size() + 8);
	for (const unsigned char ch : text)
	{
		switch (ch)
		{
			case '\"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (ch < 0x20)
				{
					out += '?';
				}
				else
				{
					out.push_back(static_cast<char>(ch));
				}
				break;
		}
	}
	return out;
}

static std::string DefaultOutputPath()
{
	const char* out_env = std::getenv("PCSX2_SIMPTRACE_OUT");
	if (out_env && out_env[0] != '\0')
		return out_env;

	const char* temp_env = std::getenv("TEMP");
	if (temp_env && temp_env[0] != '\0')
		return Path::Combine(temp_env, "pcsx2_simpskate_trace.jsonl");

	return "pcsx2_simpskate_trace.jsonl";
}

static void ParseTracepoints(const std::string_view raw)
{
	s_tracepoint_count = 0;
	std::string current;
	for (const char ch : raw)
	{
		if (ch == ',')
		{
			current = Trim(current);
			if (!current.empty() && s_tracepoint_count < s_tracepoints.size())
			{
				char* end = nullptr;
				const u32 addr = static_cast<u32>(std::strtoul(current.c_str(), &end, 0));
				if (end != nullptr && end != current.c_str())
					s_tracepoints[s_tracepoint_count++] = addr;
			}
			current.clear();
			continue;
		}
		current.push_back(ch);
	}

	current = Trim(current);
	if (!current.empty() && s_tracepoint_count < s_tracepoints.size())
	{
		char* end = nullptr;
		const u32 addr = static_cast<u32>(std::strtoul(current.c_str(), &end, 0));
		if (end != nullptr && end != current.c_str())
			s_tracepoints[s_tracepoint_count++] = addr;
	}
}

static bool EnvTruthy(const char* value, bool default_value)
{
	if (!value || value[0] == '\0')
		return default_value;
	if (std::strcmp(value, "0") == 0 || StringUtil::compareNoCase(value, "false") || StringUtil::compareNoCase(value, "no"))
		return false;
	return true;
}

static u32 ParseU32Env(const char* value, u32 default_value)
{
	if (!value || value[0] == '\0')
		return default_value;
	char* end = nullptr;
	const unsigned long parsed = std::strtoul(value, &end, 0);
	if (!end || end == value)
		return default_value;
	return static_cast<u32>(parsed);
}

static u64 ParseU64Env(const char* value, u64 default_value)
{
	if (!value || value[0] == '\0')
		return default_value;
	char* end = nullptr;
	const unsigned long long parsed = std::strtoull(value, &end, 0);
	if (!end || end == value)
		return default_value;
	return static_cast<u64>(parsed);
}

static void ParseTraceRanges(const std::string_view raw)
{
	s_trace_range_count = 0;
	std::string current;
	auto flush = [&]() {
		current = Trim(current);
		if (current.empty() || s_trace_range_count >= s_trace_ranges.size())
		{
			current.clear();
			return;
		}

		const size_t dash = current.find('-');
		if (dash == std::string::npos)
		{
			current.clear();
			return;
		}

		std::string lhs = Trim(current.substr(0, dash));
		std::string rhs = Trim(current.substr(dash + 1));
		char* end_l = nullptr;
		char* end_r = nullptr;
		const u32 start = static_cast<u32>(std::strtoul(lhs.c_str(), &end_l, 0));
		const u32 end = static_cast<u32>(std::strtoul(rhs.c_str(), &end_r, 0));
		if (end_l && end_l != lhs.c_str() && end_r && end_r != rhs.c_str())
		{
			s_trace_ranges[s_trace_range_count++] = PcRange{std::min(start, end), std::max(start, end)};
		}
		current.clear();
	};

	for (const char ch : raw)
	{
		if (ch == ',')
		{
			flush();
			continue;
		}
		current.push_back(ch);
	}
	flush();
}

static std::string HexFromBytes(const std::vector<u8>& bytes)
{
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(bytes.size() * 2);
	for (const u8 b : bytes)
	{
		out.push_back(kHex[(b >> 4) & 0xF]);
		out.push_back(kHex[b & 0xF]);
	}
	return out;
}

static std::string AsciiPreview(const std::vector<u8>& bytes)
{
	std::string out;
	out.reserve(bytes.size());
	for (const u8 b : bytes)
	{
		out.push_back((b >= 32 && b < 127) ? static_cast<char>(b) : '.');
	}
	return out;
}

static void InitLocked()
{
	if (s_init)
		return;
	s_init = true;

	const char* enabled_env = std::getenv("PCSX2_SIMPTRACE");
	if (!enabled_env || enabled_env[0] == '\0' || std::strcmp(enabled_env, "0") == 0 ||
		StringUtil::compareNoCase(enabled_env, "false"))
	{
		s_enabled = false;
		return;
	}

	std::string point_list;
	if (const char* points_env = std::getenv("PCSX2_SIMPTRACE_EE_FUNCS");
		points_env && points_env[0] != '\0')
	{
		point_list = points_env;
	}
	else
	{
		// Loader/dispatch conversion chain from current Simpsons Skateboarding reverse work.
		point_list = "0x00131410,0x00131510,0x0013e510,0x0013e910,0x0011c4d8,0x002d5150,0x0030fd00";
	}
	ParseTracepoints(point_list);
	const char* ranges_env = std::getenv("PCSX2_SIMPTRACE_EE_RANGES");
	if (ranges_env)
	{
		std::string ranges_value = Trim(ranges_env);
		if (ranges_value.empty() || ranges_value == "0" || StringUtil::compareNoCase(ranges_value, "false") ||
			StringUtil::compareNoCase(ranges_value, "off") || StringUtil::compareNoCase(ranges_value, "none"))
		{
			s_trace_range_count = 0;
		}
		else
		{
			ParseTraceRanges(ranges_value);
		}
	}
	else
	{
		// Broad neighborhoods around menu->dispatch and DAT/object conversion logic.
		ParseTraceRanges("0x00130000-0x00141fff,0x002d0000-0x002d9fff,0x0030f000-0x00311fff");
	}

	s_log_ptr_windows = EnvTruthy(std::getenv("PCSX2_SIMPTRACE_LOG_PTR_WINDOWS"), true);
	s_log_parser_snapshots = EnvTruthy(std::getenv("PCSX2_SIMPTRACE_LOG_PARSER_SNAPSHOTS"), true);
	s_log_object_candidates = EnvTruthy(std::getenv("PCSX2_SIMPTRACE_LOG_OBJECT_CANDIDATES"), true);
	s_ptr_window_bytes = std::clamp(ParseU32Env(std::getenv("PCSX2_SIMPTRACE_PTR_WINDOW_BYTES"), 96u), 16u, 4096u);
	s_stack_words = std::clamp(ParseU32Env(std::getenv("PCSX2_SIMPTRACE_STACK_WORDS"), 32u), 0u, 1024u);
	s_max_events = ParseU64Env(std::getenv("PCSX2_SIMPTRACE_MAX_EVENTS"), 0);
	s_seq = 0;
	s_limit_reached = false;

	const std::string out_path = DefaultOutputPath();
	s_fp = FileSystem::OpenCFile(out_path.c_str(), "ab");
	if (!s_fp)
	{
		Console.Error("SimpSkateTrace: failed to open '%s' (errno=%d)", out_path.c_str(), errno);
		s_enabled = false;
		return;
	}

	s_enabled = true;
	std::ostringstream os = NewJsonStream();
	os << "{\"type\":\"trace_start\",\"note\":\"simpskate\",\"ee_tracepoints\":[";
	for (size_t i = 0; i < s_tracepoint_count; i++)
	{
		if (i != 0)
			os << ',';
		os << s_tracepoints[i];
	}
	os << "],\"ee_trace_ranges\":[";
	for (size_t i = 0; i < s_trace_range_count; i++)
	{
		if (i != 0)
			os << ',';
		os << '[' << s_trace_ranges[i].start << ',' << s_trace_ranges[i].end << ']';
	}
	os << "],\"log_ptr_windows\":" << (s_log_ptr_windows ? "true" : "false")
	   << ",\"log_parser_snapshots\":" << (s_log_parser_snapshots ? "true" : "false")
	   << ",\"log_object_candidates\":" << (s_log_object_candidates ? "true" : "false")
	   << ",\"ptr_window_bytes\":" << s_ptr_window_bytes
	   << ",\"stack_words\":" << s_stack_words
	   << ",\"max_events\":" << s_max_events
	   << "}\n";
	const std::string line = os.str();
	std::fwrite(line.data(), 1, line.size(), s_fp);
	std::fflush(s_fp);
	Console.WriteLn(Color_StrongGreen, "SimpSkateTrace enabled -> %s", out_path.c_str());
}

static void LogLineLocked(const std::string& line)
{
	if (!s_enabled || !s_fp)
		return;
	std::fwrite(line.data(), 1, line.size(), s_fp);
	std::fwrite("\n", 1, 1, s_fp);
	std::fflush(s_fp);
}

static std::string ReadEEString(u32 ee_addr, size_t max_len = 160)
{
	std::string out;
	if (ee_addr == 0)
		return out;

	out.reserve(max_len);
	for (size_t i = 0; i < max_len; i++)
	{
		const u8* ptr = reinterpret_cast<const u8*>(PSM(ee_addr + static_cast<u32>(i)));
		if (!ptr)
			break;
		const unsigned char ch = *ptr;
		if (ch == 0)
			break;
		out.push_back((ch >= 32 && ch < 127) ? static_cast<char>(ch) : '?');
	}
	return out;
}

static bool ReadEEBytes(u32 ee_addr, size_t size, std::vector<u8>* out)
{
	if (!out)
		return false;
	out->clear();
	if (ee_addr == 0 || size == 0)
		return false;
	out->reserve(size);
	for (size_t i = 0; i < size; i++)
	{
		const u8* ptr = reinterpret_cast<const u8*>(PSM(ee_addr + static_cast<u32>(i)));
		if (!ptr)
			return false;
		out->push_back(*ptr);
	}
	return true;
}

static bool ReadEEU32(u32 ee_addr, u32* out)
{
	const u8* ptr = reinterpret_cast<const u8*>(PSM(ee_addr));
	if (!ptr)
		return false;
	std::memcpy(out, ptr, sizeof(u32));
	return true;
}

static bool ReadEEU16(u32 ee_addr, u16* out)
{
	const u8* ptr = reinterpret_cast<const u8*>(PSM(ee_addr));
	if (!ptr)
		return false;
	std::memcpy(out, ptr, sizeof(u16));
	return true;
}

static bool ReadEEU8(u32 ee_addr, u8* out)
{
	const u8* ptr = reinterpret_cast<const u8*>(PSM(ee_addr));
	if (!ptr)
		return false;
	*out = *ptr;
	return true;
}

static bool ReadEEF32(u32 ee_addr, float* out)
{
	u32 word = 0;
	if (!ReadEEU32(ee_addr, &word))
		return false;
	std::memcpy(out, &word, sizeof(float));
	return std::isfinite(*out);
}

static bool FloatLooksWorldScale(float v)
{
	return std::isfinite(v) && std::fabs(v) < 200000.0f;
}

static int ScoreObjectCandidate(u32 object_addr, std::string* out_custom, u16* out_status, u16* out_uid, u32* out_flags, float* out_x, float* out_y, float* out_z, float* out_rx, float* out_ry, float* out_rz)
{
	u16 status = 0;
	u16 unique_id = 0;
	u32 flags = 0;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float rx = 0.0f;
	float ry = 0.0f;
	float rz = 0.0f;
	const bool ok_status = ReadEEU16(object_addr + 0x32, &status);
	const bool ok_uid = ReadEEU16(object_addr + 0x34, &unique_id);
	const bool ok_flags = ReadEEU32(object_addr + 0x38, &flags);
	const bool ok_x = ReadEEF32(object_addr + 0x3C, &x);
	const bool ok_y = ReadEEF32(object_addr + 0x40, &y);
	const bool ok_z = ReadEEF32(object_addr + 0x44, &z);
	const bool ok_rx = ReadEEF32(object_addr + 0x48, &rx);
	const bool ok_ry = ReadEEF32(object_addr + 0x4C, &ry);
	const bool ok_rz = ReadEEF32(object_addr + 0x50, &rz);
	const std::string custom = ReadEEString(object_addr + 0x54, 192);

	int score = 0;
	if (ok_status)
		score += 5;
	if (ok_uid)
		score += 5;
	if (ok_flags)
		score += 5;
	if (ok_x && FloatLooksWorldScale(x))
		score += 12;
	if (ok_y && FloatLooksWorldScale(y))
		score += 12;
	if (ok_z && FloatLooksWorldScale(z))
		score += 12;
	if (ok_rx && std::fabs(rx) < 10000.0f)
		score += 6;
	if (ok_ry && std::fabs(ry) < 10000.0f)
		score += 6;
	if (ok_rz && std::fabs(rz) < 10000.0f)
		score += 6;

	const std::string custom_lower = StringUtil::toLower(custom);
	if (custom_lower.find(".dff") != std::string::npos || custom_lower.find("models\\") != std::string::npos || custom_lower.find("models/") != std::string::npos)
		score += 24;
	if (custom_lower.find("school") != std::string::npos || custom_lower.find("krus") != std::string::npos || custom_lower.find("ksign") != std::string::npos)
		score += 18;

	if (out_custom)
		*out_custom = custom;
	if (out_status)
		*out_status = status;
	if (out_uid)
		*out_uid = unique_id;
	if (out_flags)
		*out_flags = flags;
	if (out_x)
		*out_x = x;
	if (out_y)
		*out_y = y;
	if (out_z)
		*out_z = z;
	if (out_rx)
		*out_rx = rx;
	if (out_ry)
		*out_ry = ry;
	if (out_rz)
		*out_rz = rz;
	return score;
}

static void AppendDatObjectFields(std::ostringstream& os, u32 object_addr)
{
	u16 status = 0;
	u16 unique_id = 0;
	u32 flags = 0;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float rx = 0.0f;
	float ry = 0.0f;
	float rz = 0.0f;

	const bool ok_status = ReadEEU16(object_addr + 0x32, &status);
	const bool ok_uid = ReadEEU16(object_addr + 0x34, &unique_id);
	const bool ok_flags = ReadEEU32(object_addr + 0x38, &flags);
	const bool ok_x = ReadEEF32(object_addr + 0x3C, &x);
	const bool ok_y = ReadEEF32(object_addr + 0x40, &y);
	const bool ok_z = ReadEEF32(object_addr + 0x44, &z);
	const bool ok_rx = ReadEEF32(object_addr + 0x48, &rx);
	const bool ok_ry = ReadEEF32(object_addr + 0x4C, &ry);
	const bool ok_rz = ReadEEF32(object_addr + 0x50, &rz);
	const std::string custom = ReadEEString(object_addr + 0x54, 192);

	os << ",\"obj\":{"
	   << "\"addr\":" << object_addr
	   << ",\"valid\":" << ((ok_status || ok_uid || ok_flags || ok_x || ok_y || ok_z || ok_rx || ok_ry || ok_rz) ? "true" : "false")
	   << ",\"status\":" << status
	   << ",\"unique_id\":" << unique_id
	   << ",\"flags\":" << flags
	   << ",\"x\":" << x
	   << ",\"y\":" << y
	   << ",\"z\":" << z
	   << ",\"rx\":" << rx
	   << ",\"ry\":" << ry
	   << ",\"rz\":" << rz
	   << ",\"custom\":\"" << JsonEscape(custom) << "\""
	   << "}";
}

static void AppendObjectCandidate(std::ostringstream& os, const char* label, u32 object_addr)
{
	std::string custom;
	u16 status = 0;
	u16 unique_id = 0;
	u32 flags = 0;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float rx = 0.0f;
	float ry = 0.0f;
	float rz = 0.0f;
	const int score = ScoreObjectCandidate(object_addr, &custom, &status, &unique_id, &flags, &x, &y, &z, &rx, &ry, &rz);

	os << "{\"label\":\"" << label << "\""
	   << ",\"addr\":" << object_addr
	   << ",\"score\":" << score
	   << ",\"status\":" << status
	   << ",\"unique_id\":" << unique_id
	   << ",\"flags\":" << flags
	   << ",\"x\":" << x
	   << ",\"y\":" << y
	   << ",\"z\":" << z
	   << ",\"rx\":" << rx
	   << ",\"ry\":" << ry
	   << ",\"rz\":" << rz
	   << ",\"custom\":\"" << JsonEscape(custom) << "\""
	   << "}";
}

static void AppendParserCandidate(std::ostringstream& os, const char* label, u32 parser_addr)
{
	u32 base = 0;
	u32 rel = 0;
	u32 span = 0;
	u32 table = 0;
	u16 slot_base = 0;
	u16 slot_idx = 0;
	u8 slot_mode = 0;
	std::vector<u8> slot_bytes;
	std::vector<u8> head_bytes;
	ReadEEU32(parser_addr + 0x0, &base);
	ReadEEU32(parser_addr + 0x4, &rel);
	ReadEEU32(parser_addr + 0x8, &span);
	ReadEEU32(parser_addr + 0xC, &table);
	ReadEEU16(parser_addr + 0x298, &slot_base);
	ReadEEU16(parser_addr + 0x29A, &slot_idx);
	ReadEEU8(parser_addr + 0x29C, &slot_mode);
	ReadEEBytes(parser_addr + 0x29C, 32, &slot_bytes);
	ReadEEBytes(parser_addr, 64, &head_bytes);

	os << "{\"label\":\"" << label << "\""
	   << ",\"addr\":" << parser_addr
	   << ",\"base\":" << base
	   << ",\"rel\":" << rel
	   << ",\"span\":" << span
	   << ",\"table\":" << table
	   << ",\"slot_base\":" << slot_base
	   << ",\"slot_idx\":" << slot_idx
	   << ",\"slot_mode\":" << static_cast<u32>(slot_mode)
	   << ",\"slot_hex\":\"" << JsonEscape(HexFromBytes(slot_bytes)) << "\""
	   << ",\"slot_ascii\":\"" << JsonEscape(AsciiPreview(slot_bytes)) << "\""
	   << ",\"head_hex\":\"" << JsonEscape(HexFromBytes(head_bytes)) << "\""
	   << "}";
}

static bool AppendPointerWindow(std::ostringstream& os, const char* label, u32 addr, size_t bytes)
{
	std::vector<u8> raw;
	if (!ReadEEBytes(addr, bytes, &raw))
		return false;
	os << "{\"label\":\"" << label << "\""
	   << ",\"addr\":" << addr
	   << ",\"hex\":\"" << JsonEscape(HexFromBytes(raw)) << "\""
	   << ",\"ascii\":\"" << JsonEscape(AsciiPreview(raw)) << "\""
	   << "}";
	return true;
}
} // namespace

bool IsEnabled()
{
	std::lock_guard<std::mutex> lock(s_lock);
	InitLocked();
	return s_enabled;
}

bool IsEETracepoint(u32 pc)
{
	std::lock_guard<std::mutex> lock(s_lock);
	InitLocked();
	if (!s_enabled)
		return false;

	for (size_t i = 0; i < s_tracepoint_count; i++)
	{
		if (s_tracepoints[i] == pc)
			return true;
	}
	for (size_t i = 0; i < s_trace_range_count; i++)
	{
		if (pc >= s_trace_ranges[i].start && pc <= s_trace_ranges[i].end)
			return true;
	}
	return false;
}

void OnEETracepoint(u32 pc)
{
	std::lock_guard<std::mutex> lock(s_lock);
	InitLocked();
	if (!s_enabled)
		return;

	if (s_max_events != 0 && s_seq >= s_max_events)
	{
		if (!s_limit_reached)
		{
			s_limit_reached = true;
			std::ostringstream limit_os = NewJsonStream();
			limit_os << "{\"type\":\"trace_limit_reached\",\"max_events\":" << s_max_events
			         << ",\"last_pc\":" << pc << "}";
			LogLineLocked(limit_os.str());
		}
		return;
	}

	const u64 seq = ++s_seq;

	const u32 a0 = cpuRegs.GPR.n.a0.UL[0];
	const u32 a1 = cpuRegs.GPR.n.a1.UL[0];
	const u32 a2 = cpuRegs.GPR.n.a2.UL[0];
	const u32 a3 = cpuRegs.GPR.n.a3.UL[0];
	const u32 v0 = cpuRegs.GPR.n.v0.UL[0];
	const u32 v1 = cpuRegs.GPR.n.v1.UL[0];
	const u32 t0 = cpuRegs.GPR.n.t0.UL[0];
	const u32 t1 = cpuRegs.GPR.n.t1.UL[0];
	const u32 t2 = cpuRegs.GPR.n.t2.UL[0];
	const u32 t3 = cpuRegs.GPR.n.t3.UL[0];
	const u32 s0 = cpuRegs.GPR.n.s0.UL[0];
	const u32 s1 = cpuRegs.GPR.n.s1.UL[0];
	const u32 sp = cpuRegs.GPR.n.sp.UL[0];
	// PCSX2 exposes the frame-pointer register as s8 in the EE GPR struct.
	const u32 fp = cpuRegs.GPR.n.s8.UL[0];
	const u32 gp = cpuRegs.GPR.n.gp.UL[0];
	const u32 ra = cpuRegs.GPR.n.ra.UL[0];

	const std::string a0_text = ReadEEString(a0);
	const std::string a1_text = ReadEEString(a1);
	const std::string a2_text = ReadEEString(a2);
	const std::string a3_text = ReadEEString(a3);
	const std::string s0_text = ReadEEString(s0);
	const std::string s1_text = ReadEEString(s1);

	bool range_hit = false;
	for (size_t i = 0; i < s_trace_range_count; i++)
	{
		if (pc >= s_trace_ranges[i].start && pc <= s_trace_ranges[i].end)
		{
			range_hit = true;
			break;
		}
	}

	std::ostringstream os = NewJsonStream();
	os << "{\"type\":\"ee_trace\""
	   << ",\"seq\":" << seq
	   << ",\"cycle\":" << cpuRegs.cycle
	   << ",\"pc\":" << pc
	   << ",\"range_hit\":" << (range_hit ? "true" : "false")
	   << ",\"a0\":" << a0
	   << ",\"a1\":" << a1
	   << ",\"a2\":" << a2
	   << ",\"a3\":" << a3
	   << ",\"v0\":" << v0
	   << ",\"v1\":" << v1
	   << ",\"t0\":" << t0
	   << ",\"t1\":" << t1
	   << ",\"t2\":" << t2
	   << ",\"t3\":" << t3
	   << ",\"s0\":" << s0
	   << ",\"s1\":" << s1
	   << ",\"sp\":" << sp
	   << ",\"fp\":" << fp
	   << ",\"gp\":" << gp
	   << ",\"ra\":" << ra
	   << ",\"a0_str\":\"" << JsonEscape(a0_text) << "\""
	   << ",\"a1_str\":\"" << JsonEscape(a1_text) << "\""
	   << ",\"a2_str\":\"" << JsonEscape(a2_text) << "\""
	   << ",\"a3_str\":\"" << JsonEscape(a3_text) << "\""
	   << ",\"s0_str\":\"" << JsonEscape(s0_text) << "\""
	   << ",\"s1_str\":\"" << JsonEscape(s1_text) << "\"";

	if (s_stack_words > 0)
	{
		const size_t stack_bytes = static_cast<size_t>(s_stack_words) * sizeof(u32);
		std::vector<u8> stack_raw;
		if (ReadEEBytes(sp, stack_bytes, &stack_raw))
		{
			os << ",\"stack\":{\"sp\":" << sp
			   << ",\"bytes\":" << stack_bytes
			   << ",\"hex\":\"" << JsonEscape(HexFromBytes(stack_raw)) << "\""
			   << ",\"ascii\":\"" << JsonEscape(AsciiPreview(stack_raw)) << "\"}";
		}
	}

	if (s_log_parser_snapshots)
	{
		os << ",\"parser_candidates\":[";
		bool first = true;
		auto append_parser = [&](const char* label, u32 addr) {
			if (addr == 0)
				return;
			if (!first)
				os << ',';
			AppendParserCandidate(os, label, addr);
			first = false;
		};
		append_parser("a0", a0);
		append_parser("a1", a1);
		append_parser("a2", a2);
		append_parser("a3", a3);
		append_parser("s0", s0);
		append_parser("s1", s1);
		append_parser("sp", sp);
		append_parser("fp", fp);
		append_parser("gp", gp);
		os << "]";
	}

	if (s_log_object_candidates)
	{
		os << ",\"object_candidates\":[";
		bool first = true;
		auto append_object = [&](const char* label, u32 addr) {
			if (addr == 0)
				return;
			if (!first)
				os << ',';
			AppendObjectCandidate(os, label, addr);
			first = false;
		};
		auto append_deref_object = [&](const char* label, u32 addr) {
			if (addr == 0)
				return;
			u32 deref = 0;
			if (!ReadEEU32(addr, &deref) || deref == 0)
				return;
			std::string label_text = "*(";
			label_text += label;
			label_text += ")";
			if (!first)
				os << ',';
			AppendObjectCandidate(os, label_text.c_str(), deref);
			first = false;
		};

		append_object("a0", a0);
		append_object("a1", a1);
		append_object("a2", a2);
		append_object("a3", a3);
		append_object("s0", s0);
		append_object("s1", s1);
		append_object("sp", sp);
		append_object("fp", fp);
		append_object("gp", gp);

		append_deref_object("a0", a0);
		append_deref_object("a1", a1);
		append_deref_object("a2", a2);
		append_deref_object("a3", a3);
		append_deref_object("s0", s0);
		append_deref_object("s1", s1);
		append_deref_object("sp", sp);
		append_deref_object("fp", fp);
		append_deref_object("gp", gp);
		os << "]";
	}

	if (s_log_ptr_windows && s_ptr_window_bytes > 0)
	{
		os << ",\"ptr_windows\":[";
		bool first = true;
		auto append_window = [&](const char* label, u32 addr) {
			if (addr == 0)
				return;
			std::ostringstream tmp = NewJsonStream();
			if (AppendPointerWindow(tmp, label, addr, s_ptr_window_bytes))
			{
				if (!first)
					os << ',';
				os << tmp.str();
				first = false;
			}
		};
		auto append_deref_window = [&](const char* label, u32 addr) {
			if (addr == 0)
				return;
			u32 deref = 0;
			if (!ReadEEU32(addr, &deref) || deref == 0)
				return;
			std::string label_text = "*(";
			label_text += label;
			label_text += ")";
			std::ostringstream tmp = NewJsonStream();
			if (AppendPointerWindow(tmp, label_text.c_str(), deref, s_ptr_window_bytes))
			{
				if (!first)
					os << ',';
				os << tmp.str();
				first = false;
			}
		};

		append_window("a0", a0);
		append_window("a1", a1);
		append_window("a2", a2);
		append_window("a3", a3);
		append_window("s0", s0);
		append_window("s1", s1);
		append_window("sp", sp);
		append_window("fp", fp);
		append_window("gp", gp);

		append_deref_window("a0", a0);
		append_deref_window("a1", a1);
		append_deref_window("a2", a2);
		append_deref_window("a3", a3);
		append_deref_window("s0", s0);
		append_deref_window("s1", s1);
		append_deref_window("sp", sp);
		append_deref_window("fp", fp);
		append_deref_window("gp", gp);
		os << "]";
	}

	// DAT object record snapshots:
	// - entry (pc=0x13e510): destination object in a1
	// - return path (pc=0x13e910): destination object persisted in s0
	if (pc == 0x0013E510 && a1 != 0)
	{
		os << ",\"phase\":\"entry\"";
		AppendDatObjectFields(os, a1);
	}
	else if (pc == 0x0013E910 && s0 != 0)
	{
		os << ",\"phase\":\"exit\"";
		AppendDatObjectFields(os, s0);
	}

	os << "}";
	LogLineLocked(os.str());
}

void OnIsoOpen(const std::string_view& iso_path)
{
	std::lock_guard<std::mutex> lock(s_lock);
	InitLocked();
	if (!s_enabled)
		return;

	std::ostringstream os = NewJsonStream();
	os << "{\"type\":\"iso_open\",\"path\":\"" << JsonEscape(std::string(iso_path)) << "\"}";
	LogLineLocked(os.str());
}

void OnIsoMapBuilt(size_t file_count, bool has_assets_blt, u32 assets_blt_lsn, u32 assets_blt_size)
{
	std::lock_guard<std::mutex> lock(s_lock);
	InitLocked();
	if (!s_enabled)
		return;

	std::ostringstream os = NewJsonStream();
	os << "{\"type\":\"iso_map\",\"files\":" << file_count
	   << ",\"has_assets_blt\":" << (has_assets_blt ? "true" : "false")
	   << ",\"assets_blt_lsn\":" << assets_blt_lsn
	   << ",\"assets_blt_size\":" << assets_blt_size
	   << "}";
	LogLineLocked(os.str());
}

void OnIsoReadRun(
	u32 start_lsn,
	u32 sector_count,
	int mode,
	u32 ee_pc,
	u32 iop_pc,
	const std::string_view& owner_path,
	u32 owner_offset,
	u32 owner_size)
{
	std::lock_guard<std::mutex> lock(s_lock);
	InitLocked();
	if (!s_enabled)
		return;

	std::ostringstream os = NewJsonStream();
	os << "{\"type\":\"iso_read\""
	   << ",\"start_lsn\":" << start_lsn
	   << ",\"sector_count\":" << sector_count
	   << ",\"mode\":" << mode
	   << ",\"ee_pc\":" << ee_pc
	   << ",\"iop_pc\":" << iop_pc
	   << ",\"owner\":\"" << JsonEscape(std::string(owner_path)) << "\""
	   << ",\"owner_offset\":" << owner_offset
	   << ",\"owner_size\":" << owner_size
	   << "}";
	LogLineLocked(os.str());
}
} // namespace SimpSkateTrace
