#define NOMINMAX

#include <filesystem>
#include <iostream>
#include <fstream>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Mod/CppMod.hpp>


namespace RC
{
    constexpr uint32_t polynomial = 0xEDB88320;
    constexpr uint32_t generate_crc(uint32_t crc, uint8_t j)
    {
        return j ? (crc & 1) ? generate_crc((crc >> 1) ^ polynomial, j - 1) : generate_crc(crc >> 1, j - 1) : crc;
    }

    constexpr uint32_t generate_table_value(uint32_t i)
    {
        return generate_crc(i, 8);
    }

    template <size_t... I>
    constexpr auto generate_table(std::index_sequence<I...>)
    {
        return std::array<uint32_t, sizeof...(I)>{generate_table_value(I)...};
    }

    constexpr auto crc32_table = generate_table(std::make_index_sequence<256>{});

    uint32_t compute_crc32(const uint8_t* data, size_t length)
    {
        uint32_t crc = 0xFFFFFFFF;
        while (length--)
        {
            uint8_t byte = *data++;
            crc = (crc >> 8) ^ crc32_table[(crc & 0xFF) ^ byte];
        }
        return ~crc;
    }

    std::wstring CppMod::temp_dlls_path = std::filesystem::temp_directory_path().wstring() + L"ue4ss\\dlls\\";


    CppMod::CppMod(UE4SSProgram& program, std::wstring&& mod_name, std::wstring&& mod_path) : Mod(program, std::move(mod_name), std::move(mod_path))
    {
        m_dlls_path = m_mod_path + L"\\dlls";

        if (!std::filesystem::exists(m_dlls_path))
        {
            Output::send<LogLevel::Warning>(STR("Could not find the dlls folder for mod {}\n"), m_mod_name);
            set_installable(false);
            return;
        }

        auto dll_path = m_dlls_path + L"\\main.dll";

        // Load file into memory
        std::ifstream file(dll_path, std::ios::binary);
        std::vector<uint8_t> buffer(std::istreambuf_iterator<char>(file), {});

        // Compute crc32 of dll
        uint32_t crc = compute_crc32(buffer.data(), buffer.size());

        // create new filename with crc32
        auto new_dll_path = temp_dlls_path + std::to_wstring(crc) + L".dll";

        // check if temp folder exists
        if (!std::filesystem::exists(temp_dlls_path))
        {
			std::filesystem::create_directories(temp_dlls_path);
		}

        // copy over dll if it doesn't exist
        if (!std::filesystem::exists(new_dll_path))
        {
			Output::send<LogLevel::Verbose>(STR("mod {} has new main.dll, copying it to <{}>\n"), m_mod_name, new_dll_path);
			std::filesystem::copy_file(dll_path, new_dll_path);
		}

        // Add mods dlls directory to search path for dynamic/shared linked libraries in mods
        m_dlls_path_cookie = AddDllDirectory(m_dlls_path.c_str());
        m_main_dll_module = LoadLibraryW(new_dll_path.c_str());

        if (!m_main_dll_module)
        {
            Output::send<LogLevel::Warning>(STR("Failed to load dll <{}> for mod {}, error code: 0x{:x}\n"), dll_path, m_mod_name, GetLastError());
            set_installable(false);
            return;
        }

        m_start_mod_func = reinterpret_cast<start_type>(GetProcAddress(m_main_dll_module, "start_mod"));
        m_uninstall_mod_func = reinterpret_cast<uninstall_type>(GetProcAddress(m_main_dll_module, "uninstall_mod"));

        if (!m_start_mod_func || !m_uninstall_mod_func)
        {
            Output::send<LogLevel::Warning>(STR("Failed to find exported mod lifecycle functions for mod {}\n"), m_mod_name);

            FreeLibrary(m_main_dll_module);
            m_main_dll_module = NULL;

            set_installable(false);
            return;
        }
    }

    auto CppMod::start_mod() -> void
    {
        try
        {
            m_mod = m_start_mod_func();
            m_is_started = m_mod != nullptr;
        }
        catch (std::exception& e)
        {
            if (!Output::has_internal_error())
            {
                Output::send<LogLevel::Warning>(STR("Failed to load dll <{}> for mod {}, because: {}\n"),
                                                m_dlls_path + L"\\main.dll\n",
                                                m_mod_name,
                                                to_wstring(e.what()));
            }
            else
            {
                printf_s("Internal Error: %s\n", e.what());
            }
        }
    }

    auto CppMod::uninstall() -> void
    {
        Output::send(STR("Stopping C++ mod '{}' for uninstall\n"), m_mod_name);
        if (m_mod && m_uninstall_mod_func)
        {
            m_uninstall_mod_func(m_mod);
        }
    }

    auto CppMod::fire_on_lua_start(StringViewType mod_name,
                                   LuaMadeSimple::Lua& lua,
                                   LuaMadeSimple::Lua& main_lua,
                                   LuaMadeSimple::Lua& async_lua,
                                   std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void
    {
        if (m_mod)
        {
            m_mod->on_lua_start(mod_name, lua, main_lua, async_lua, hook_luas);
        }
    }

    auto CppMod::fire_on_lua_start(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua, std::vector<LuaMadeSimple::Lua*>& hook_luas)
            -> void
    {
        if (m_mod)
        {
            m_mod->on_lua_start(lua, main_lua, async_lua, hook_luas);
        }
    }

    auto CppMod::fire_on_lua_stop(StringViewType mod_name,
                                  LuaMadeSimple::Lua& lua,
                                  LuaMadeSimple::Lua& main_lua,
                                  LuaMadeSimple::Lua& async_lua,
                                  std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void
    {
        if (m_mod)
        {
            m_mod->on_lua_stop(mod_name, lua, main_lua, async_lua, hook_luas);
        }
    }

    auto CppMod::fire_on_lua_stop(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua, std::vector<LuaMadeSimple::Lua*>& hook_luas)
            -> void
    {
        if (m_mod)
        {
            m_mod->on_lua_stop(lua, main_lua, async_lua, hook_luas);
        }
    }

    auto CppMod::fire_unreal_init() -> void
    {
        if (m_mod)
        {
            m_mod->on_unreal_init();
        }
    }

    auto CppMod::fire_program_start() -> void
    {
        if (m_mod)
        {
            m_mod->on_program_start();
        }
    }

    auto CppMod::fire_update() -> void
    {
        if (m_mod)
        {
            m_mod->on_update();
        }
    }

    auto CppMod::fire_dll_load(std::wstring_view dll_name) -> void
    {
        if (m_mod)
        {
            m_mod->on_dll_load(dll_name);
        }
    }

    CppMod::~CppMod()
    {
        if (m_main_dll_module)
        {
            FreeLibrary(m_main_dll_module);
            RemoveDllDirectory(m_dlls_path_cookie);
        }
    }
} // namespace RC