-- When pasting into a terminal, drop trailing line breaks so a pasted
-- command waits for you to press Enter instead of running at once.

local terminals = { "ghostty", "kitty", "alacritty", "foot", "wezterm", "konsole", "terminal" }

local function is_terminal(app)
  app = (app or ""):lower()
  for _, t in ipairs(terminals) do
    if app:find(t, 1, true) then return true end
  end
  return false
end

function on_paste(clip, target)
  if not is_terminal(target.app) or not clip.text then return end
  local trimmed = clip.text:gsub("[\r\n]+$", "")
  if trimmed ~= clip.text and trimmed ~= "" then return trimmed end
end
