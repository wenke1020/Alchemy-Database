module("whitelist", package.seeall);

function set(key, val)
  --print ('set');
  return redis('set', key, val);
end
function get(key)
  --print ('get');
  return redis('get', key);
end

function subscribe(channel)
  --print ('subscribe');
  return redis('subscribe', channel);
end
function publish(channel, msg)
  print ('publish');
  return redis('publish', channel, msg);
end