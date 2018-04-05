xw = require("xwiilua")
devices = xw.xwii_list()
print("got " .. #devices .. " devices")
for i,v in ipairs(devices) do
   print(i, v)
end

d = xw.xwii_open(1)
print("opened device #", d)

function print_data(msg, data)
   if data ~= nil then
      print(msg, unpack(data))
   end
end

function print_motion(a, m, na, ns)
   print_data("accel:", a)
   print_data("motion+:", m)
   print_data("NC accel:", na)
   print_data("NC stick:", ns)
end

function poll()
   local lastev = 0
   ev,a,m,na,ns = xw.xwii_poll(d)
   print_motion(a,m,na,ns)
   while ev ~= nil do
      print("got", unpack(ev))
      ev,a,m,na,ns = xw.xwii_poll(d)
      print_motion(a,m,na,ns)
   end
end
