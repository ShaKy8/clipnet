-- Strip tracking parameters from copied links.
--
--   https://shop.example/item?id=7&utm_source=news&fbclid=abc
--   → https://shop.example/item?id=7
--
-- Only clips that are a single URL are touched; anything else is left alone.

local tracking = {
  "utm_[%w_]+", "fbclid", "gclid", "dclid", "gbraid", "wbraid", "msclkid",
  "mc_cid", "mc_eid", "igshid", "si", "_hsenc", "_hsmi", "mkt_tok", "yclid",
}

local function is_tracking(key)
  for _, pattern in ipairs(tracking) do
    if key:match("^" .. pattern .. "$") then return true end
  end
  return false
end

function on_copy(clip)
  local text = clip.text
  if not text then return end
  local url = text:match("^%s*(https?://%S+)%s*$")
  if not url then return end

  local base, query, fragment = url:match("^([^?#]*)%??([^#]*)(#?.*)$")
  if query == "" then return end
  local kept = {}
  for pair in query:gmatch("[^&]+") do
    local key = pair:match("^([^=]*)")
    if not is_tracking(key) then kept[#kept + 1] = pair end
  end
  local cleaned = base .. (#kept > 0 and ("?" .. table.concat(kept, "&")) or "") .. fragment
  if cleaned ~= url then return { text = cleaned } end
end
