-- lua_sexp.lua -- s-expression reader/writer for the Hard Truck 2 .sch data.
--
-- Counterpart of tools/sexp.py.  Data files under tools/out/lua are plain Lua
-- tables emitted by tools/sch2lua.py; this module turns them back into the
-- canonical s-expression text, which is what makes the sch -> lua -> sch round
-- trip checkable byte for byte (tools/sch2lua_check.sh):
--
--     local lua_sexp = require "lua_sexp"
--     local forms = dofile "out/lua/env_mod_aa.sch.lua"
--     io.write (lua_sexp.to_text (forms))
--
-- Generated data shape (what tools/sch2lua.py writes):
--     form    { head = "town", <field>, <field>, ... }
--     field   plain string      a bare atom, printed verbatim
--             { q = "..." }     an atom the original wrote as "..."
--             table             a nested list / nested form, recursed
-- An empty list is `{}`.  Scalar text is UTF-8 (Lua source is UTF-8); `to_text`
-- transcodes the quoted atoms to cp1251, which is what .sch bytes are.
--
-- Parsed-node shape (what M.parse returns from raw .sch bytes):
--     atom    { text = "...", quoted = true|false }
--     list    { <node>, <node>, ... } with `head` set for a plain first atom

local M = {}

local byte, char, sub, find, format = string.byte, string.char,
    string.sub, string.find, string.format

-- ---------------------------------------------------------------------------
-- cp1251 <-> UTF-8 (the 128 high bytes differ)
-- ---------------------------------------------------------------------------
local CP1251_HIGH = {
  [0x80] = "\208\130",
  [0x81] = "\208\131",
  [0x82] = "\226\128\154",
  [0x83] = "\209\147",
  [0x84] = "\226\128\158",
  [0x85] = "\226\128\166",
  [0x86] = "\226\128\160",
  [0x87] = "\226\128\161",
  [0x88] = "\226\130\172",
  [0x89] = "\226\128\176",
  [0x8A] = "\208\137",
  [0x8B] = "\226\128\185",
  [0x8C] = "\208\138",
  [0x8D] = "\208\140",
  [0x8E] = "\208\139",
  [0x8F] = "\208\143",
  [0x90] = "\209\146",
  [0x91] = "\226\128\152",
  [0x92] = "\226\128\153",
  [0x93] = "\226\128\156",
  [0x94] = "\226\128\157",
  [0x95] = "\226\128\162",
  [0x96] = "\226\128\147",
  [0x97] = "\226\128\148",
  [0x99] = "\226\132\162",
  [0x9A] = "\209\153",
  [0x9B] = "\226\128\186",
  [0x9C] = "\209\154",
  [0x9D] = "\209\156",
  [0x9E] = "\209\155",
  [0x9F] = "\209\159",
  [0xA0] = "\194\160",
  [0xA1] = "\208\142",
  [0xA2] = "\209\158",
  [0xA3] = "\208\136",
  [0xA4] = "\194\164",
  [0xA5] = "\210\144",
  [0xA6] = "\194\166",
  [0xA7] = "\194\167",
  [0xA8] = "\208\129",
  [0xA9] = "\194\169",
  [0xAA] = "\208\132",
  [0xAB] = "\194\171",
  [0xAC] = "\194\172",
  [0xAD] = "\194\173",
  [0xAE] = "\194\174",
  [0xAF] = "\208\135",
  [0xB0] = "\194\176",
  [0xB1] = "\194\177",
  [0xB2] = "\208\134",
  [0xB3] = "\209\150",
  [0xB4] = "\210\145",
  [0xB5] = "\194\181",
  [0xB6] = "\194\182",
  [0xB7] = "\194\183",
  [0xB8] = "\209\145",
  [0xB9] = "\226\132\150",
  [0xBA] = "\209\148",
  [0xBB] = "\194\187",
  [0xBC] = "\209\152",
  [0xBD] = "\208\133",
  [0xBE] = "\209\149",
  [0xBF] = "\209\151",
  [0xC0] = "\208\144",
  [0xC1] = "\208\145",
  [0xC2] = "\208\146",
  [0xC3] = "\208\147",
  [0xC4] = "\208\148",
  [0xC5] = "\208\149",
  [0xC6] = "\208\150",
  [0xC7] = "\208\151",
  [0xC8] = "\208\152",
  [0xC9] = "\208\153",
  [0xCA] = "\208\154",
  [0xCB] = "\208\155",
  [0xCC] = "\208\156",
  [0xCD] = "\208\157",
  [0xCE] = "\208\158",
  [0xCF] = "\208\159",
  [0xD0] = "\208\160",
  [0xD1] = "\208\161",
  [0xD2] = "\208\162",
  [0xD3] = "\208\163",
  [0xD4] = "\208\164",
  [0xD5] = "\208\165",
  [0xD6] = "\208\166",
  [0xD7] = "\208\167",
  [0xD8] = "\208\168",
  [0xD9] = "\208\169",
  [0xDA] = "\208\170",
  [0xDB] = "\208\171",
  [0xDC] = "\208\172",
  [0xDD] = "\208\173",
  [0xDE] = "\208\174",
  [0xDF] = "\208\175",
  [0xE0] = "\208\176",
  [0xE1] = "\208\177",
  [0xE2] = "\208\178",
  [0xE3] = "\208\179",
  [0xE4] = "\208\180",
  [0xE5] = "\208\181",
  [0xE6] = "\208\182",
  [0xE7] = "\208\183",
  [0xE8] = "\208\184",
  [0xE9] = "\208\185",
  [0xEA] = "\208\186",
  [0xEB] = "\208\187",
  [0xEC] = "\208\188",
  [0xED] = "\208\189",
  [0xEE] = "\208\190",
  [0xEF] = "\208\191",
  [0xF0] = "\209\128",
  [0xF1] = "\209\129",
  [0xF2] = "\209\130",
  [0xF3] = "\209\131",
  [0xF4] = "\209\132",
  [0xF5] = "\209\133",
  [0xF6] = "\209\134",
  [0xF7] = "\209\135",
  [0xF8] = "\209\136",
  [0xF9] = "\209\137",
  [0xFA] = "\209\138",
  [0xFB] = "\209\139",
  [0xFC] = "\209\140",
  [0xFD] = "\209\141",
  [0xFE] = "\209\142",
  [0xFF] = "\209\143",
}

local UTF8_TO_CP1251 = {}
for i = 0x80, 0xFF do
  local s = CP1251_HIGH[i]
  if s then UTF8_TO_CP1251[s] = i end
end

local SENTINEL = 0x110000   -- out of cp1251 range: "no byte"

-- utf8_to_cp1251: UTF-8 text -> cp1251 byte string.
function M.utf8_to_cp1251 (s)
  local out, n, i = {}, #s, 1
  while i <= n do
    local b = byte(s, i)
    local nb, cp
    if b < 0xC0 then nb, cp = 1, b
    elseif b < 0xE0 then nb, cp = 2, b - 0xC0
    elseif b < 0xF0 then nb, cp = 3, b - 0xE0
    else nb, cp = 4, b - 0xF0 end
    local seq = sub(s, i, i + nb - 1)
    local ok = true
    for k = 2, nb do
      local c = byte(s, i + k - 1)
      if not c or c < 0x80 or c > 0xBF then ok = false break end
      cp = cp * 64 + (c - 0x80)
    end
    if not ok or cp > 0x10FFFF then
      error("lua_sexp: invalid UTF-8 input", 0)
    end
    local mapped
    if cp < 0x80 then mapped = cp else mapped = UTF8_TO_CP1251[seq] end
    if not mapped then
      error("lua_sexp: character U+" .. format("%04X", cp) ..
            " is not representable in cp1251", 0)
    end
    out[#out + 1] = char(mapped)
    i = i + nb
  end
  return table.concat(out)
end

-- cp1251_to_utf8: cp1251 byte string -> UTF-8 text.
function M.cp1251_to_utf8 (s)
  local out = {}
  for k = 1, #s do
    local b = byte(s, k)
    if b < 0x80 then out[k] = char(b) else out[k] = CP1251_HIGH[b] end
  end
  return table.concat(out)
end

-- ---------------------------------------------------------------------------
-- parsing raw .sch bytes
-- ---------------------------------------------------------------------------
local NUMBER_CHARS = {}
for c in ("+-.eE0123456789"):gmatch(".") do NUMBER_CHARS[c] = true end

-- True when `s` is a bare number (digits first, then only number characters).
function M.is_number (s)
  if s == "" or s == "." then return false end
  for i = 1, #s do
    if not NUMBER_CHARS[sub(s, i, i)] then return false end
  end
  return tonumber(s) ~= nil
end

-- True when `s` cannot be written as a bare atom, so it needs quotes.
function M.needs_quote (s)
  if s == "" then return true end
  if find(s, "[%s()]") then return true end
  if find(s, ";", 1, true) then return true end
  local c = sub(s, 1, 1)
  if c == "\"" or c == "\\" then return true end
  return false
end

local WS = { [" "] = true, ["\t"] = true, ["\r"] = true,
             ["\n"] = true, ["\f"] = true, ["\v"] = true }

-- Parse one node starting at `start`; returns node, next index.
function M.parse_token (s, start)
  local b = byte(s, start)
  if b == 0x28 then                       -- (
    local kids, i, n = {}, start + 1, #s
    while true do
      while i <= n and WS[sub(s, i, i)] do i = i + 1 end
      if i > n then error("lua_sexp: unbalanced (", 0) end
      if sub(s, i, i) == ")" then i = i + 1 break end
      local kid
      kid, i = M.parse_token(s, i)
      kids[#kids + 1] = kid
    end
    local first = kids[1]
    if first and first.text ~= nil and not first.quoted then
      kids.head = first.text              -- a plain first atom, as emitted
    else
      kids.nohead = true
    end
    return kids, i
  end
  if b == 0x22 then                       -- "
    local out, i, n = {}, start + 1, #s
    while true do
      if i > n then error("lua_sexp: unterminated string", 0) end
      if sub(s, i, i) == "\"" then i = i + 1 break end
      out[#out + 1] = sub(s, i, i)
      i = i + 1
    end
    return { text = table.concat(out), quoted = true }, i
  end
  local i, n = start, #s
  while i <= n do
    local c = sub(s, i, i)
    if WS[c] or c == "(" or c == ")" then break end
    i = i + 1
  end
  return { text = sub(s, start, i - 1), quoted = false }, i
end

-- Parse a whole document (raw cp1251 bytes) -> list of top-level nodes.
function M.parse (data)
  local forms, i, n = {}, 1, #data
  while i <= n do
    local c = sub(data, i, i)
    if WS[c] then
      i = i + 1
    elseif c == ";" then
      while i <= n and not WS[sub(data, i, i)] do i = i + 1 end
    else
      local form
      form, i = M.parse_token(data, i)
      forms[#forms + 1] = form
    end
  end
  return forms
end

-- Load a raw s-expression file (decrypted .sch text).
function M.load (path)
  local f = assert(io.open(path, "rb"))
  local data = f:read("a")
  f:close()
  return M.parse(data)
end

-- ---------------------------------------------------------------------------
-- canonical dump
-- ---------------------------------------------------------------------------
-- Backslash and double quote, as *values* (never as Lua escapes).
local BS = string.char(92)
local DQ = string.char(34)

-- Escape a cp1251 byte string.
--
--   * every byte from 0x20 up passes through as itself, **including the
--     cp1251 Cyrillic range 0xC0..0xFF**: that is what the .sch data contains
--     and what the reader expects back
--   * `"`, `\` and the controls below 0x20 become short escapes
--   * the remaining control bytes become decimal escapes, zero padded when the
--     next byte is a digit so that `\10` followed by `5` cannot re-read as `\105`
local function esc (bytes)
  local data = type(bytes) == "string" and bytes or table.concat(bytes)
  local out, n = {}, #data
  for k = 1, n do
    local b = byte(data, k)
    local c = char(b)
    if b == 0x22 then
      out[k] = BS .. DQ
    elseif b == 0x5C then
      out[k] = BS .. BS
    elseif b == 0x0A then
      out[k] = BS .. "n"
    elseif b == 0x0D then
      out[k] = BS .. "r"
    elseif b == 0x09 then
      out[k] = BS .. "t"
    elseif b >= 0x20 and b <= 0xFF and b ~= 0x22 and b ~= 0x5C then
      out[k] = c
    else
      local nb = k < n and byte(data, k + 1) or nil
      if nb and nb > 0x2F and nb < 0x3A then
        out[k] = BS .. format("%03d", b)
      else
        out[k] = BS .. format("%d", b)
      end
    end
  end
  return table.concat(out)
end

local function dump_quote (cp1251)
  return DQ .. esc(cp1251) .. DQ
end

-- A parsed atom: print exactly as it was read.
local function atom_text (node)
  if node.quoted then return dump_quote(node.text) end
  return node.text
end

local dump_node

-- One field of a generated form:
--   plain string      a bare atom (printed verbatim)
--   { q = "..." }     an atom the original wrote as "..." (UTF-8 -> cp1251)
--   table             a nested list / nested form, recursed
--   number            hand-written data only; printed canonically
local function dump_field (v)
  local t = type(v)
  if t == "string" then return v end
  if t == "table" then
    if v.q ~= nil then return dump_quote(M.utf8_to_cp1251(v.q)) end
    return dump_node(v)
  end
  if t == "number" then
    if v == math.floor(v) and math.abs(v) < 2 ^ 53 then
      return format("%.0f", v)
    end
    return format("%.17g", v)
  end
  error("lua_sexp: cannot dump a " .. t .. " field", 0)
end

dump_node = function (node)
  if node.quoted ~= nil then return atom_text(node) end
  local out = {}
  if node.head ~= nil and not node.nohead then out[1] = node.head end
  if node.text ~= nil then
    error("lua_sexp: ambiguous table (both text and head)", 0)
  end
  local i = 1
  while node[i] ~= nil do
    out[#out + 1] = dump_field(node[i])
    i = i + 1
  end
  return "(" .. table.concat(out, " ") .. ")"
end

-- Canonical text of a list of forms (or of one node).
function M.to_text (forms)
  if type(forms) == "table" and forms.text ~= nil then return dump_node(forms) end
  local out = {}
  for k = 1, #forms do out[k] = dump_node(forms[k]) end
  return table.concat(out, "\r\n") .. "\r\n"
end

M.dump = M.to_text

-- Canonical cp1251 bytes of a list of forms.
function M.to_bytes (forms)
  return M.utf8_to_cp1251(M.to_text(forms))
end

return M
