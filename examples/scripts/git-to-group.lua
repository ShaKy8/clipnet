-- File git remote URLs and `git clone` commands in the "Code ▸ Git" group.

local patterns = {
  "^%s*git clone ",
  "^%s*git@[%w%.%-]+:[%w%._%-/]+%.git%s*$",
  "^%s*https://github%.com/[%w%._%-]+/[%w%._%-]+%.git%s*$",
  "^%s*https://gitlab%.com/.+%.git%s*$",
}

function on_copy(clip)
  local text = clip.text
  if not text then return end
  for _, p in ipairs(patterns) do
    if text:match(p) then return { group = "Code/Git" } end
  end
end
