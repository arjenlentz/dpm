-- Hello-world style initialization script for proxy.

-- Name -> value defines for lua
dofile ("defines.lua")

callback = {}
clients  = {}
storage  = {}

passdb   = {["whee"] = "09A4298405EF045A61DB26DF8811FEA0E44A80FD"}

function client_ok(cid)
    print("Client ready!", cid)
end

function client_got_auth(auth_pkt, cid)
    print("Got auth callback", type(auth_pkt), type(cid))
    local hs_pkt = storage[cid]
    if (passdb[auth_pkt:user()] and myp.check_pass(auth_pkt, hs_pkt, passdb[auth_pkt:user()]) == 0) then
        print "OMFG passwords matched!"
        local ok_pkt = myp.new_ok_pkt()
        -- FIXME: Prior to this stage "Client waiting" should mean "Client got
        -- auth"
        callback[cid] = {["Client waiting"] = client_ok}
        myp.wire_packet(clients[cid], ok_pkt)
    else
        print "OMFG passwords did NOT match!!!"
        local err_pkt = myp.new_err_pkt()
        callback[cid] = nil
        myp.wire_packet(clients[cid], err_pkt)
        clients[cid] = nil
    end

    storage[cid] = nil
end

function new_client(c)
    -- "c" is a new listening connection object.
    print("Holy crap it's a new client!", type(c), c:id(), c:listener(), c:my_type())
    clients[c:id()] = c -- Prevent client from being garbage collected
    callback[c:id()] = {["Client waiting"] = client_got_auth}

    local hs_pkt = myp.new_handshake_pkt()
    print("Built a new handshake packet!", type(hs_pkt), hs_pkt:protocol_version(), hs_pkt:server_version())
    myp.wire_packet(c, hs_pkt)
    storage[c:id()] = hs_pkt
end

function server_handshake(hs_pkt, cid)
    print "Got callback for server handshake packet"
    print("HS_PKT", type(hs_pkt), hs_pkt:protocol_version(), hs_pkt:server_version(), cid)
end

listen = myp.listener("127.0.0.1", 5500)
print("listener data: ", listen:id(), listen:listener())
callback[listen:id()] = {["Client connect"] = new_client}

backend = myp.connect("127.0.0.1", 3306)
callback[backend:id()] = {["Server waiting auth"] = server_handshake}

