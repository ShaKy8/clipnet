-- Don't keep one-time codes (4–8 digits on their own, e.g. 482913) in the
-- history. They still paste normally: this only stops them being stored.

function on_copy(clip)
  local text = clip.text
  if text and text:match("^%s*%d%d%d%d%d?%d?%d?%d?%s*$") then
    return false
  end
end
