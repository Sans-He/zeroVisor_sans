

import sys
import glob
import subprocess
import os
import shutil

kernel_version = []
symbol_table_array = []

# Search system.map directory and make tables.
def make_symbol_table_in_directory():
    filelist = glob.glob("./src/system.map/*")

    # Extract kernel version
    for item in filelist:
        item = item.replace("./src/system.map/", "")
        item = item.replace(".map", "")
        kernel_version.append(item)

    # Make symbol table
    for item in filelist:
        output = get_symbol_table_from_file(item)
        symbol_table_array.append(output);

# Extact symbol from system.map
def get_symbol_table_from_file(filename):
    symbol_list = [
        "_text",
        "_etext",
        "__start___ex_table",
        "__stop___ex_table",
        "__start_rodata",
        "__end_rodata",
        "modules",
        "tasklist_lock",
        "init_mm",
        "wake_up_new_task",
        "ftrace_module_init",
        "free_module",
        "walk_system_ram_range",
    ]

    symbol_table = []

    fp_input = open(filename, "r")
    data_list = fp_input.readlines()

    found = 0
    for data in data_list:
        data_item = data.split(" ");

        data_item[2] = data_item[2].replace("\n", "")
        for symbol in symbol_list:
            if (symbol == data_item[2]):
                found = found + 1
                symbol_table.append([data_item[2], data_item[0]])


    if (found != len(symbol_list)):
        print("    [WARNING] %s symbol find fail" % filename)
    else:
        print("    [SUCCESS] %s symbol find success" % filename)

    return symbol_table

def find_kallsyms_lookup_name_from_proc():
    try:
        with open("/proc/kallsyms", "r") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 3 and parts[2] == "kallsyms_lookup_name":
                    return parts[0]
    except Exception as e:
        print(f"[ERROR] Failed to read /proc/kallsyms: {e}")
    return None

def auto_copy_system_map():
    """Automatically copy current kernel's System.map file"""
    try:
        # Get kernel version using uname -v
        result = subprocess.run(['uname', '-v'], capture_output=True, text=True)
        if result.returncode != 0:
            print("[ERROR] Failed to get kernel version")
            return False
        
        current_kernel_version = result.stdout.strip()
        print(f"[INFO] Current kernel version: {current_kernel_version}")
        
        # Get kernel release for system.map filename
        result = subprocess.run(['uname', '-r'], capture_output=True, text=True)
        if result.returncode != 0:
            print("[ERROR] Failed to get kernel release")
            return False
        
        kernel_release = result.stdout.strip()
        source_file = f"/boot/System.map-{kernel_release}"
        
        # Check if source file exists
        if not os.path.exists(source_file):
            print(f"[ERROR] System.map file not found: {source_file}")
            return False
        
        # Create system.map directory if it doesn't exist
        os.makedirs("./src/system.map", exist_ok=True)
        
        # Destination filename using kernel version
        dest_file = f"./src/system.map/{current_kernel_version}"
        
        # Copy the file
        shutil.copy2(source_file, dest_file)
        print(f"[SUCCESS] Copied {source_file} to {dest_file}")
        
        return True
        
    except Exception as e:
        print(f"[ERROR] Failed to auto copy system.map: {e}")
        return False



# main
if __name__ == "__main__":
    if not auto_copy_system_map():
        print("[ERROR] Auto copy failed, please manually copy System.map file")

    fp_output = open("./include/symbol.h", "w")
    
    # Try read kallsyms_lookup_name address from /proc/kallsyms
    kallsyms_addr = find_kallsyms_lookup_name_from_proc()
    if kallsyms_addr:
        print(f"[INFO] Found kallsyms_lookup_name at 0x{kallsyms_addr}")
        fp_output.write("// Dynamically added from /proc/kallsyms\n")
        fp_output.write(f"#define KALLSYMS_LOOKUP_NAME_ADDR 0x{kallsyms_addr}\n")
    else:
        print("[WARNING] Failed to find kallsyms_lookup_name in /proc/kallsyms")
    
    make_symbol_table_in_directory()

    # Write kernel version.
    fp_output.write("char* g_kernel_version[] = \n{\n");
    for item in kernel_version:
        fp_output.write('\t"%s",\n' % item)
    fp_output.write("};\n\n");

    # Write symbol table.
    fp_output.write("struct zv_symbol_table g_symbol_table_array[] =\n{\n");
    table_index = 0
    for item in symbol_table_array:
        fp_output.write("\t//%s\n" %  kernel_version[table_index])
        fp_output.write("\t{\n")
        fp_output.write("\t\t{\n")

        for table in item:
            fp_output.write('\t\t\t{"%s", 0x%s},\n' % (table[0], table[1]))

        fp_output.write("\t\t},\n")
        fp_output.write("\t},\n")

        table_index = table_index + 1
    fp_output.write("};\n\n");
    sys.exit(0)
