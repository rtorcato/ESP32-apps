"""Upload Home to `factory`, not to `ota_0`.

The platform's builder picks the upload offset from the partition table and
prefers ota_0 when the table has one (builder/main.py, "if partition[subtype]
== ota_0: app_offset = next_offset"). That default is exactly right for every
app here -- `pio run -e ticker -t upload` lands in the app slot with no
configuration at all -- and exactly wrong for the one firmware that has to
live in factory.

Runs as a post: script so it overwrites the offset the platform just set.
"""
Import("env")  # noqa: F821  (injected by SCons)

FACTORY_OFFSET = "0x10000"

# subst, not get: the platform Replaces it into the env and get() reads None.
was = env.subst("$ESP32_APP_OFFSET")
env.Replace(ESP32_APP_OFFSET=FACTORY_OFFSET)
print(f"home_offset: upload offset {was} -> {FACTORY_OFFSET} (factory)")
