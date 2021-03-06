#include <windows.h>
#include <stdio.h>
#include <string>   

typedef BOOL (WINAPI *DllMainFunction)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved);
typedef HMODULE(WINAPI *LoadLibraryFunction) (LPCSTR lpLibFileName);
typedef LPVOID  (WINAPI *VirtualAllocFunction) (LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
typedef FARPROC(WINAPI *GetProcAddressFunction) (HMODULE hModule, LPCSTR lpProcName);
typedef BOOL(WINAPI *VirtualFreeFunction)(LPVOID lpAddress,	SIZE_T dwSize, DWORD  dwFreeType);
typedef BOOL(WINAPI *VirtualProtectFunction)(LPVOID lpAddress, SIZE_T dwSize, DWORD  flNewProtect, PDWORD lpflOldProtect);

#ifdef WIN_X86
	typedef unsigned long PtrInt;
#elif WIN_X64
	typedef unsigned long long PtrInt;
#endif

typedef struct 
{
	byte* raw_module_destination;
	// i dont want the shellcode to search for the functions if the injector already has functionality to do this
	LoadLibraryFunction fnLoadLibrary;	 	
	GetProcAddressFunction fnGetProcAddress;
	VirtualAllocFunction fnVirtualAlloc;
	VirtualFreeFunction fnVirtualFree;
	VirtualProtectFunction fnVirtualProtect;
} ShellcodeInformation;

typedef struct
{
	WORD	offset : 12;
	WORD	type : 4;
} IMAGE_RELOC;

DWORD WINAPI LoadLibShellcode(ShellcodeInformation* modinfo)
{		   
	IMAGE_DOS_HEADER* idh = (IMAGE_DOS_HEADER*)modinfo->raw_module_destination;

	if (idh->e_magic != IMAGE_DOS_SIGNATURE) {
		return 0;
	}			
					
	IMAGE_NT_HEADERS* headers = (IMAGE_NT_HEADERS*)(modinfo->raw_module_destination + idh->e_lfanew);
	   
	if (headers->Signature != IMAGE_NT_SIGNATURE) {
		return 0;
	}	
					
	// reserve memory for the complete module
	byte* module_destination = (byte*)modinfo->fnVirtualAlloc((LPVOID)headers->OptionalHeader.ImageBase, headers->OptionalHeader.SizeOfImage, MEM_RESERVE, PAGE_READWRITE);
	//byte* module_destination = 0;
	if (!module_destination)
	{
		module_destination = (byte*)modinfo->fnVirtualAlloc(0, headers->OptionalHeader.SizeOfImage, MEM_RESERVE, PAGE_READWRITE);
	}

	//
	// Map headers into memory
	//	  
	modinfo->fnVirtualAlloc(module_destination, headers->OptionalHeader.SizeOfHeaders, MEM_COMMIT, PAGE_READWRITE);
	for (SIZE_T index = 0; index < headers->OptionalHeader.SizeOfHeaders; index++)
	{
		module_destination[index] = modinfo->raw_module_destination[index];
	}	   
	// Update pointer to headers
	IMAGE_NT_HEADERS* new_headers = (IMAGE_NT_HEADERS*)(module_destination + idh->e_lfanew);


	//
	// Map sections into memory
	//						
	for (int section_counter = 0; section_counter < new_headers->FileHeader.NumberOfSections; section_counter++)
	{

		IMAGE_SECTION_HEADER* ish = (IMAGE_SECTION_HEADER *)((PtrInt)&new_headers->OptionalHeader + new_headers->FileHeader.SizeOfOptionalHeader + section_counter * sizeof(IMAGE_SECTION_HEADER));
		
		byte* section_destination = 0;
		if (ish->SizeOfRawData != 0)
		{
			// commit the memory for this section
			section_destination = (byte*)modinfo->fnVirtualAlloc(module_destination + ish->VirtualAddress, ish->SizeOfRawData, MEM_COMMIT, PAGE_READWRITE);
														 
			// copy section into final location
			for (SIZE_T index = 0; index < ish->SizeOfRawData; index++)
			{
				(module_destination + ish->VirtualAddress)[index] = (modinfo->raw_module_destination + ish->PointerToRawData)[index];
			}
		}
		else
		{
			// Zero the empty areas just in case
			if (ish->Characteristics == IMAGE_SCN_CNT_INITIALIZED_DATA)
			{
				section_destination = (byte*)modinfo->fnVirtualAlloc(module_destination + ish->VirtualAddress, new_headers->OptionalHeader.SizeOfInitializedData, MEM_COMMIT, PAGE_READWRITE);

				for (SIZE_T index = 0; index < new_headers->OptionalHeader.SizeOfInitializedData; index++)
				{
					(module_destination + ish->VirtualAddress)[index] = 0;
				}
				ish->SizeOfRawData = new_headers->OptionalHeader.SizeOfInitializedData;
			}
			else if (ish->Characteristics == IMAGE_SCN_CNT_UNINITIALIZED_DATA)
			{
				section_destination = (byte*)modinfo->fnVirtualAlloc(module_destination + ish->VirtualAddress, new_headers->OptionalHeader.SizeOfUninitializedData, MEM_COMMIT, PAGE_READWRITE);

				for (SIZE_T index = 0; index < new_headers->OptionalHeader.SizeOfUninitializedData; index++)
				{
					(module_destination + ish->VirtualAddress)[index] = 0;
				}	
				ish->SizeOfRawData = new_headers->OptionalHeader.SizeOfUninitializedData;
			}	 			
		}			  
	}


	//
	// Perform Relocations
	//	 
	PtrInt delta = (PtrInt)(module_destination - new_headers->OptionalHeader.ImageBase);

	if (delta != 0)
	{
		IMAGE_DATA_DIRECTORY* relocation_directory_entry = &new_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
		IMAGE_BASE_RELOCATION* relocation_table = (IMAGE_BASE_RELOCATION*)(module_destination + relocation_directory_entry->VirtualAddress);

		while (relocation_table->VirtualAddress)
		{
			IMAGE_RELOC* relocation_block = (IMAGE_RELOC*)((PtrInt)relocation_table + sizeof(IMAGE_BASE_RELOCATION));
			for (DWORD relocations = 0; relocations < ((relocation_table->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2); relocations++)
			{
				IMAGE_RELOC reloc_info = relocation_block[relocations];

				if (reloc_info.type == IMAGE_REL_BASED_HIGHLOW)
				{
					*(DWORD*)(module_destination + relocation_table->VirtualAddress + reloc_info.offset) += delta;
				}
				else if (reloc_info.type == IMAGE_REL_BASED_DIR64)
				{
					*(UINT64*)(module_destination + relocation_table->VirtualAddress + reloc_info.offset) += delta;
				}	  
			}
			relocation_table = (IMAGE_BASE_RELOCATION*)((PtrInt)relocation_table + relocation_table->SizeOfBlock);
		}
	}

	//
	// Resolve Imports
	//
	IMAGE_DATA_DIRECTORY* import_directory_entry = &new_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	IMAGE_IMPORT_DESCRIPTOR* import_table = (IMAGE_IMPORT_DESCRIPTOR*)(module_destination + import_directory_entry->VirtualAddress);

	for (int import_module_index = 0; import_module_index  < ((import_directory_entry->Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) - 1)); import_module_index++)
	{
		HMODULE import_module = modinfo->fnLoadLibrary((char*)((PtrInt)module_destination + import_table[import_module_index].Name));
			   
		if (!import_module){
			return 0;
		}

		// http://win32assembly.programminghorizon.com/pe-tut6.html
		IMAGE_THUNK_DATA* originalThunkTable = (IMAGE_THUNK_DATA*)(module_destination + import_table[import_module_index].OriginalFirstThunk);
		IMAGE_THUNK_DATA* thunkTable = (IMAGE_THUNK_DATA*)(module_destination + import_table[import_module_index].FirstThunk);

		if (!originalThunkTable)
		{
			originalThunkTable = thunkTable;
		}
		
		for (int import_function_index = 0; thunkTable[import_function_index].u1.Function != 0 /*the table is zero terminated*/; import_function_index++) 
		{
			if (IMAGE_SNAP_BY_ORDINAL(originalThunkTable[import_function_index].u1.Ordinal))
			{
				//by ordinal
				WORD ordinal = IMAGE_ORDINAL(originalThunkTable[import_function_index].u1.Ordinal);
				thunkTable[import_function_index].u1.Function = (PtrInt)modinfo->fnGetProcAddress(import_module, MAKEINTRESOURCE(ordinal));
			}
			else
			{
				// by name
				IMAGE_IMPORT_BY_NAME* thunkdata = (IMAGE_IMPORT_BY_NAME*)(module_destination + originalThunkTable[import_function_index].u1.AddressOfData);   				
				thunkTable[import_function_index].u1.Function = (PtrInt)modinfo->fnGetProcAddress(import_module, thunkdata->Name);		   		   
			}
			if (thunkTable[import_function_index].u1.Function == 0)
			{
				return 0;
			}
		}
	}

	//
	// Set Section Memory flags
	//
	//						
	for (int section_counter = 0; section_counter < new_headers->FileHeader.NumberOfSections; section_counter++)
	{
		IMAGE_SECTION_HEADER* ish = (IMAGE_SECTION_HEADER *)((PtrInt)&new_headers->OptionalHeader + new_headers->FileHeader.SizeOfOptionalHeader + section_counter * sizeof(IMAGE_SECTION_HEADER));
		DWORD memoryprotection = 0;

		if (ish->Characteristics & IMAGE_SCN_MEM_DISCARDABLE)
		{
			modinfo->fnVirtualFree((LPVOID)ish->Misc.PhysicalAddress, ish->SizeOfRawData, MEM_DECOMMIT);
			continue;
		}  

		if (ish->Characteristics & IMAGE_SCN_MEM_EXECUTE) 
		{
			if (ish->Characteristics & IMAGE_SCN_MEM_READ) 
			{
				if (ish->Characteristics & IMAGE_SCN_MEM_WRITE)	
				{
					memoryprotection = PAGE_EXECUTE_READWRITE;
				}											  
				else 
				{
					memoryprotection = PAGE_EXECUTE_READ;
				}
			}
			else 
			{	  
				if (ish->Characteristics & IMAGE_SCN_MEM_WRITE)	
				{
					memoryprotection = PAGE_EXECUTE_WRITECOPY;
				}
				else 
				{									  
					memoryprotection = PAGE_EXECUTE;
				}
			} 			
		}
		else 
		{	   
			if (ish->Characteristics & IMAGE_SCN_MEM_READ) 
			{
				if (ish->Characteristics & IMAGE_SCN_MEM_WRITE)	
				{
					memoryprotection = PAGE_READWRITE;
				}
				else 
				{
					memoryprotection = PAGE_READONLY;
				}
			}
			else 
			{
				if (ish->Characteristics & IMAGE_SCN_MEM_WRITE)	
				{
					memoryprotection = PAGE_WRITECOPY;
				}
				else 
				{
					memoryprotection = PAGE_NOACCESS;
				}
			}
		}

		if (ish->Characteristics & IMAGE_SCN_MEM_NOT_CACHED)
		{
			memoryprotection |= PAGE_NOCACHE;
		}

		DWORD oldprot;
		if (!modinfo->fnVirtualProtect((LPVOID)(module_destination + ish->VirtualAddress), ish->SizeOfRawData, memoryprotection, &oldprot))
		{	
			return 0;
		}		   
	}	
	
	//
	// call Entrypoint
	//
	PtrInt dllmainaddress = (PtrInt)module_destination + new_headers->OptionalHeader.AddressOfEntryPoint;	
	((DllMainFunction)dllmainaddress)((HINSTANCE)module_destination, DLL_PROCESS_ATTACH, NULL);
																	
	return delta;
}	  	  	 
					   
void END_SHELLCODE(void) {}

void Test()
{
#ifdef WIN_X86
	const char* dll = "E:\\Project\\Dlls\\test.dll";
#endif

#ifdef WIN_X64
	const char* dll = "E:\\Project\\Dlls\\test.dll";
#endif // WIN_X64

	ShellcodeInformation* info = new ShellcodeInformation();

	FILE* file;
	fopen_s(&file, dll, "rb");
	fseek(file, 0, SEEK_END);          // Jump to the end of the file
	SIZE_T filelen = ftell(file);      // Get the current byte offset in the file
	rewind(file);

	byte* buffer = new byte[filelen + 1];
	fread(buffer, filelen, 1, file); // Read in the entire file
	fclose(file); // Close the file

	info->raw_module_destination = buffer;
	info->fnLoadLibrary = (LoadLibraryFunction)LoadLibraryA;
	info->fnVirtualAlloc = (VirtualAllocFunction)VirtualAlloc;
	info->fnVirtualFree = (VirtualFreeFunction)VirtualFree;
	info->fnGetProcAddress = (GetProcAddressFunction)GetProcAddress;
	info->fnVirtualProtect = (VirtualProtectFunction)VirtualProtect;


	LoadLibShellcode(info);
	LoadLibShellcode(info);
	LoadLibShellcode(info);
	LoadLibShellcode(info);
}


void CreateCSharpByteArray()
{
#ifdef WIN_X86
	const char* fileName = "charp_array_x86.cs";
#endif

#ifdef WIN_X64
	const char* fileName = "charp_array_x64.cs";
#endif // WIN_X64

	long codelength = (long)END_SHELLCODE - (long)LoadLibShellcode;

	FILE *output_file;
	fopen_s(&output_file, fileName, "w");

	
#ifdef WIN_X86
	std::string decl = "byte[] shellcode_x86 = new byte[] {";
#endif

#ifdef WIN_X64
	std::string decl = "byte[] shellcode_x64 = new byte[] {";
#endif // WIN_X64

	char buf[5];
	for (long idx = 0; idx < codelength; idx++)
	{ 	
		int num = (int)((byte*)LoadLibShellcode)[idx];
		//decl += std::to_string(num);

		sprintf_s(buf, "%X", num);
		decl += "0x";
		decl += buf;
		if(idx != codelength -1)
			decl += ", ";  
	}

	decl += "};";

	fwrite(decl.c_str(), decl.length(), 1, output_file);

	fclose(output_file);
}

void CreateBinary()
{
	FILE *output_file;

#ifdef WIN_X86
	const char* fileName = "shellcode_x86.bin";
#endif

#ifdef WIN_X64
	const char* fileName = "shellcode_x64.bin";
#endif // WIN_X64

	fopen_s(&output_file, fileName, "w");
	fwrite(LoadLibShellcode, (long)END_SHELLCODE - (long)LoadLibShellcode, 1, output_file);
	fclose(output_file);
}

int main(int argc, char *argv[])
{		



#ifdef _DEBUG
	Test(); 
#else					
	// make sure all breakpoints are deleted
	CreateCSharpByteArray();
#endif
    return 0;
}