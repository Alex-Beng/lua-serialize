local Lockbox = {};

Lockbox.ALLOW_INSECURE = true;

Lockbox.insecure = function()
    assert(Lockbox.ALLOW_INSECURE,
            "This module is insecure!  It should not be used in production." ..
            "If you really want to use it, set Lockbox.ALLOW_INSECURE to true before importing it");
end

return Lockbox;
