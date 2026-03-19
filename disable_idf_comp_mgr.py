Import("env")
import os

# Disable ESP-IDF component manager so it doesn't re-fetch and overwrite
# patched managed_components (e.g. espressif__esp_insights CMakeLists.txt)
os.environ["IDF_COMPONENT_MANAGER"] = "0"
