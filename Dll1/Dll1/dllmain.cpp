#include <windows.h>
#include <iostream>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "pch.h"
#include "dumper/dumper.hpp"

auto startup()
{
	AllocConsole();
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	FILE* output;
	freopen_s(&output, "CONOUT$", "w", stdout);

	return output;
}

DWORD cleanup(HMODULE module, FILE* output)
{
	fclose(output);
	FreeConsole();
	FreeLibraryAndExitThread(module, 0);

	return 0;
}

DWORD WINAPI entry_point(HMODULE module)
{
	auto output{ startup() };
	printf("[memity] console started\n");

	api::init();
	printf("[memity] api initalized\n");

	const auto game = std::make_unique<Dumper>();
	printf("[memity] images dumped\n");

	const auto image = game->get_image("Assembly-CSharp.dll");
	printf("[memity] Assembly-CSharp -> %s (0x%llx)\n", image->get_name(), reinterpret_cast<uintptr_t>(image));

	HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
	char class_name[256];
	char input[256];
	DWORD charsRead;
	uintptr_t instance_address = 0;

	while (true)
	{
		printf("[memity] Enter class name (or 'exit' to quit): ");

		if (!ReadConsoleA(hStdin, class_name, sizeof(class_name) - 1, &charsRead, nullptr))
			break;

		// Remove carriage return and newline
		class_name[charsRead] = '\0';
		char* newline = strchr(class_name, '\r');
		if (newline) *newline = '\0';
		newline = strchr(class_name, '\n');
		if (newline) *newline = '\0';

		if (strcmp(class_name, "exit") == 0)
			break;

		if (strlen(class_name) == 0)
			continue;

		const auto target_class = image->get_class(class_name);
		if (!target_class)
		{
			printf("[memity] Class '%s' not found\n", class_name);
			continue;
		}

		printf("[memity] %s -> %s (0x%llx)\n", class_name, target_class->get_name(), reinterpret_cast<uintptr_t>(target_class));

		// Ask for instance address
		printf("[memity] Enter instance address (hex, or 0 to skip): ");
		if (!ReadConsoleA(hStdin, input, sizeof(input) - 1, &charsRead, nullptr))
			break;

		input[charsRead] = '\0';
		newline = strchr(input, '\r');
		if (newline) *newline = '\0';
		newline = strchr(input, '\n');
		if (newline) *newline = '\0';

		instance_address = 0;
		if (strlen(input) > 0)
		{
			instance_address = strtoull(input, nullptr, 16);
		}

		for (const auto field : target_class->get_fields())
		{
			const auto name = api::get_field_name(field);
			// Skip obfuscated/non-ASCII field names
			bool is_readable = true;
			for (const char* c = name; *c; c++)
			{
				if (static_cast<unsigned char>(*c) > 127)
				{
					is_readable = false;
					break;
				}
			}
			if (is_readable)
			{
				const auto type = api::get_field_type(field);
				const auto type_name = api::get_type_name(type);
				const auto offset = target_class->get_field_offset(name);

				printf("\t[memity] %s (0x%zx) %s", name, offset, type_name ? type_name : "unknown");

				// Read and display value if instance address is provided
				if (instance_address != 0)
				{
					void* field_ptr = reinterpret_cast<void*>(instance_address + offset);

					if (strcmp(type_name, "System.Single") == 0) // float
					{
						float val = *reinterpret_cast<float*>(field_ptr);
						printf(" %f", val);
					}
					else if (strcmp(type_name, "System.Int32") == 0) // int
					{
						int32_t val = *reinterpret_cast<int32_t*>(field_ptr);
						printf(" %d", val);
					}
					else if (strcmp(type_name, "System.UInt32") == 0) // uint
					{
						uint32_t val = *reinterpret_cast<uint32_t*>(field_ptr);
						printf(" %u", val);
					}
					else if (strcmp(type_name, "System.Boolean") == 0) // bool
					{
						bool val = *reinterpret_cast<bool*>(field_ptr);
						printf(" %s", val ? "true" : "false");
					}
					else if (strcmp(type_name, "System.Int64") == 0) // long
					{
						int64_t val = *reinterpret_cast<int64_t*>(field_ptr);
						printf(" %lld", val);
					}
					else if (strcmp(type_name, "System.UInt64") == 0) // ulong
					{
						uint64_t val = *reinterpret_cast<uint64_t*>(field_ptr);
						printf(" %llu", val);
					}
					else if (strcmp(type_name, "System.Double") == 0) // double
					{
						double val = *reinterpret_cast<double*>(field_ptr);
						printf(" %lf", val);
					}
					else if (strcmp(type_name, "System.Byte") == 0) // byte
					{
						uint8_t val = *reinterpret_cast<uint8_t*>(field_ptr);
						printf(" %u", val);
					}
					else if (strcmp(type_name, "System.SByte") == 0) // sbyte
					{
						int8_t val = *reinterpret_cast<int8_t*>(field_ptr);
						printf(" %d", val);
					}
					else if (strcmp(type_name, "System.Int16") == 0) // short
					{
						int16_t val = *reinterpret_cast<int16_t*>(field_ptr);
						printf(" %d", val);
					}
					else if (strcmp(type_name, "System.UInt16") == 0) // ushort
					{
						uint16_t val = *reinterpret_cast<uint16_t*>(field_ptr);
						printf(" %u", val);
					}
				}

				printf("\n");
			}
		}

		// Allow setting values if instance address is provided
		if (instance_address != 0)
		{
			printf("[memity] Set value? (format: field_name value, or 'skip'): ");
			if (!ReadConsoleA(hStdin, input, sizeof(input) - 1, &charsRead, nullptr))
				break;

			input[charsRead] = '\0';
			newline = strchr(input, '\r');
			if (newline) *newline = '\0';
			newline = strchr(input, '\n');
			if (newline) *newline = '\0';

			if (strcmp(input, "skip") == 0 || strlen(input) == 0)
			{
				printf("\n");
				continue;
			}

			// Parse field_name and value
			char* space = strchr(input, ' ');
			if (space)
			{
				*space = '\0';
				char* field_to_set = input;
				char* value_str = space + 1;

				const auto field = api::get_field(target_class, field_to_set);
				if (field)
				{
					const auto type = api::get_field_type(field);
					const auto type_name = api::get_type_name(type);
					const auto offset = target_class->get_field_offset(field_to_set);
					void* field_ptr = reinterpret_cast<void*>(instance_address + offset);

					if (strcmp(type_name, "System.Single") == 0) // float
					{
						float val = static_cast<float>(atof(value_str));
						*reinterpret_cast<float*>(field_ptr) = val;
						printf("[memity] Set %s to %f\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.Int32") == 0) // int
					{
						int32_t val = static_cast<int32_t>(atoi(value_str));
						*reinterpret_cast<int32_t*>(field_ptr) = val;
						printf("[memity] Set %s to %d\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.UInt32") == 0) // uint
					{
						uint32_t val = static_cast<uint32_t>(strtoul(value_str, nullptr, 10));
						*reinterpret_cast<uint32_t*>(field_ptr) = val;
						printf("[memity] Set %s to %u\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.Boolean") == 0) // bool
					{
						bool val = (strcmp(value_str, "true") == 0);
						*reinterpret_cast<bool*>(field_ptr) = val;
						printf("[memity] Set %s to %s\n", field_to_set, val ? "true" : "false");
					}
					else if (strcmp(type_name, "System.Int64") == 0) // long
					{
						int64_t val = static_cast<int64_t>(strtoll(value_str, nullptr, 10));
						*reinterpret_cast<int64_t*>(field_ptr) = val;
						printf("[memity] Set %s to %lld\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.UInt64") == 0) // ulong
					{
						uint64_t val = static_cast<uint64_t>(strtoull(value_str, nullptr, 10));
						*reinterpret_cast<uint64_t*>(field_ptr) = val;
						printf("[memity] Set %s to %llu\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.Double") == 0) // double
					{
						double val = atof(value_str);
						*reinterpret_cast<double*>(field_ptr) = val;
						printf("[memity] Set %s to %lf\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.Byte") == 0) // byte
					{
						uint8_t val = static_cast<uint8_t>(atoi(value_str));
						*reinterpret_cast<uint8_t*>(field_ptr) = val;
						printf("[memity] Set %s to %u\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.SByte") == 0) // sbyte
					{
						int8_t val = static_cast<int8_t>(atoi(value_str));
						*reinterpret_cast<int8_t*>(field_ptr) = val;
						printf("[memity] Set %s to %d\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.Int16") == 0) // short
					{
						int16_t val = static_cast<int16_t>(atoi(value_str));
						*reinterpret_cast<int16_t*>(field_ptr) = val;
						printf("[memity] Set %s to %d\n", field_to_set, val);
					}
					else if (strcmp(type_name, "System.UInt16") == 0) // ushort
					{
						uint16_t val = static_cast<uint16_t>(strtoul(value_str, nullptr, 10));
						*reinterpret_cast<uint16_t*>(field_ptr) = val;
						printf("[memity] Set %s to %u\n", field_to_set, val);
					}
					else
					{
						printf("[memity] Unsupported type: %s\n", type_name);
					}
				}
				else
				{
					printf("[memity] Field '%s' not found\n", field_to_set);
				}
			}
		}
		printf("\n");
	}
	return cleanup(module, output);
}

DWORD WINAPI DllMain(HINSTANCE module,
	DWORD reason,
	void* reserved
)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		const auto thread = CreateThread(nullptr, 0x0, reinterpret_cast<LPTHREAD_START_ROUTINE>(entry_point), module, 0x0, nullptr);
		if (thread)
		{
			DisableThreadLibraryCalls(module);
			CloseHandle(thread);
		}
	}
	return TRUE;
}


