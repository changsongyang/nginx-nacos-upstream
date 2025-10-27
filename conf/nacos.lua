
local nacos = require "nacos";
local cjson = require "cjson.safe";

local projects = {}
local list_un = nil

local function project_handler(v)
    nacos.log(1, "project_handler  receiving hosts ", v.service_name, " group= ", v.group, " len ", #v)

end

local function project_list_handler(v)
    nacos.log(1, "project_list_handler content_len=", #v.data .." md5= ", v.md5, "data_id", v.data_id, "group= ", v.group)
    local project_list = cjson.decode(v.data)

     -- 取消订阅在 project_list 中不存在的项目
    local to_unsubscribe = {}
    for project, _ in pairs(projects) do
        to_unsubscribe[project] = true
    end

    if project_list then
        for _, item in ipairs(project_list.routes) do
            to_unsubscribe[item.service] = nil
            if not projects[item.service] then
                projects[item.service] = nacos.subscribe_service(item.service, "", project_handler)
                nacos.log(1, "subscribe service  ", item.service)
            end

        end
    elseif list_un then
        nacos.log(1, "unsubscribe data_id= ", list_un.data_id, " group= ", list_un.group)
        list_un()
        list_un = nil
        collectgarbage()
    else
        nacos.log(1, "project_list_handler no project_list")
    end


    for project, _ in pairs(to_unsubscribe) do
        nacos.log(1, "unsubscribe service  ", project)
        local unsubscribe = projects[project]
        projects[project] = nil
        unsubscribe();
    end

end

local function main(v)

    local m = nacos.listen_config("gateway.server.route.json", "p1", project_list_handler)
    m = nacos.listen_config("gateway.server.route.json", "p1", project_list_handler)
    m = nacos.listen_config("gateway.server.route.json", "stg", project_list_handler)
    local mt = getmetatable(m)
    if mt then
        nacos.log(1, "found meta")

        if mt.__tostring then
            nacos.log(1, "found tostring " .. tostring(m))
        else
            nacos.log(1, "not found tostring")
        end
    else
        nacos.log(1, "not found meta")
    end

    nacos.log(1, "started data_id = " .. m.data_id .. " group = " .. m.group .. " desc = " .. tostring(m))
    return m
end


list_un = main();
