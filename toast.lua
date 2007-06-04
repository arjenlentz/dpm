-- Hello-world style initialization script for proxy.

-- Name -> value defines for lua
dofile ("defines.lua")

callback = {}
clients  = {}
storage  = {}

passdb   = {["whee"] = "09A4298405EF045A61DB26DF8811FEA0E44A80FD"}

function client_got_auth(auth_pkt, cid)
    print("Got auth callback", type(auth_pkt), type(cid))
    print("Auth pkt:", auth_pkt:user(), auth_pkt:charset_number(), auth_pkt:max_packet_size())
    local hs_pkt = storage[cid]
    print("Old handshake packet!", type(hs_pkt), hs_pkt:protocol_version(), hs_pkt:server_version())
    if (passdb[auth_pkt:user()] and myp.check_pass(auth_pkt, hs_pkt, passdb[auth_pkt:user()]) == 0) then
        print "OMFG passwords matched!"
    else
        print "OMFG passwords did NOT match!!!"
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

listen = myp.listener("127.0.0.1", 5500)
print("listener data: ", listen:id(), listen:listener())
callback[listen:id()] = {["Client connect"] = new_client}
-- listen2 = myp.listener("127.0.0.1", 5501)
-- print("listener2 data: ", listen2:id(), listen2:listener())
-- callback[listen2:id()] = {["Client connect"] = new_client}

