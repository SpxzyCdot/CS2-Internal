
import os
import sys

def convert_image_to_header(image_path, output_path, array_name="esp_preview1"):
    try:
        with open(image_path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        print(f"Error: {image_path} not found. Please place the image in the same directory.")
        return False

    hex_data = ", ".join([f"0x{b:02X}" for b in data])
    
    header_content = f"unsigned char {array_name}[] = {{\n"
    # Split into lines of 16 bytes for readability
    lines = [hex_data[i:i+96] for i in range(0, len(hex_data), 96)] # 16 bytes * 6 chars per byte = 96
    
    # Fix the splitting logic to be cleaner (comma separation)
    formatted_lines = []
    current_line = []
    for i, byte in enumerate(data):
        current_line.append(f"0x{byte:02X}")
        if (i + 1) % 16 == 0:
            formatted_lines.append(", ".join(current_line) + ",")
            current_line = []
    
    if current_line:
        formatted_lines.append(", ".join(current_line))

    header_content += "\n".join(formatted_lines)
    header_content += "\n};\n"

    with open(output_path, "w") as f:
        f.write(header_content)
    
    print(f"Successfully converted {image_path} to {output_path}")
    return True

if __name__ == "__main__":
    # convert preview.png to hooks/texture.h
    target_img = "preview.png"
    target_header = "hooks/texture.h"
    
    if os.path.exists(target_img):
        convert_image_to_header(target_img, target_header)
    else:
        print(f"No {target_img} found. Skipping update.")
