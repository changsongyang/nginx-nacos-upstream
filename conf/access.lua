local cjson = require "cjson";
local a = cjson.decode(ngx.var.n_var);
ngx.header["x-echo"] = "access"..a.routes[1].path
