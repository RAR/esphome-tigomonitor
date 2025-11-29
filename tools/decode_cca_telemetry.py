#!/usr/bin/env python3
"""
CCA 12-Byte Telemetry Decoder
Decodes raw binary telemetry data from Tigo CCA (Cloud Connect Advanced)

Format verified against 31 production samples (100% match)
See CCA_BINARY_ANALYSIS.md for complete documentation

Usage:
  ./decode_cca_telemetry.py <hex_string>
  ./decode_cca_telemetry.py 503025322105B084FF443F8F

Output format matches lmudcd.dump.rawdata columns
"""

import sys


def decode_telemetry(hex_str):
    """
    Decode 12-byte CCA telemetry format
    
    Returns: dict with slot, vin, iin, temp, pwm, vout
    """
    data = bytes.fromhex(hex_str)
    
    if len(data) != 12:
        raise ValueError(f"Expected 12 bytes, got {len(data)}")
    
    return {
        'slot': data[2],                                      # 8-bit
        'vin': (data[3] << 4) | (data[4] >> 4),              # 12-bit
        'iin': (data[5] & 0x0F) << 4 | (data[6] >> 4),       # 8-bit spanning bytes
        'temp': data[7],                                      # 8-bit
        'pwm': data[8],                                       # 8-bit (0-255)
        'vout': ((data[9] & 0x40) << 2) | data[11],         # 9-bit: byte[9].bit6 + byte[11]
        'unknown': {
            'byte0': data[0],
            'byte1': data[1],
            'byte6_upper': data[6] >> 4,
            'byte9_other': data[9] & 0xBF,  # All bits except bit 6
            'byte10': data[10],
        }
    }


def format_output(result):
    """Format decoded result for display"""
    print(f"Decoded Telemetry:")
    print(f"  Slot:  0x{result['slot']:02X} ({result['slot']})")
    print(f"  Vin:   0x{result['vin']:03X} ({result['vin']}) - Input voltage (raw)")
    print(f"  Iin:   0x{result['iin']:02X} ({result['iin']}) - Input current (raw)")
    print(f"  Temp:  0x{result['temp']:02X} ({result['temp']}) - Temperature (raw)")
    print(f"  PWM:   0x{result['pwm']:02X} ({result['pwm']}) - Duty cycle 0-255")
    print(f"  Vout:  0x{result['vout']:03X} ({result['vout']}) - Output voltage (raw)")
    print()
    print(f"Unknown bytes:")
    print(f"  byte[0]:  0x{result['unknown']['byte0']:02X}")
    print(f"  byte[1]:  0x{result['unknown']['byte1']:02X}")
    print(f"  byte[6]>>4: 0x{result['unknown']['byte6_upper']:X}")
    print(f"  byte[9]&~0x40: 0x{result['unknown']['byte9_other']:02X}")
    print(f"  byte[10]: 0x{result['unknown']['byte10']:02X}")
    print()
    print("Note: Raw values require version-dependent scaling for real units")
    print("See CCA_BINARY_ANALYSIS.md for scaling formulas")


def main():
    if len(sys.argv) != 2:
        print("Usage: decode_cca_telemetry.py <24_hex_chars>")
        print("Example: decode_cca_telemetry.py 503025322105B084FF443F8F")
        sys.exit(1)
    
    hex_input = sys.argv[1].replace(' ', '').replace(':', '')
    
    try:
        result = decode_telemetry(hex_input)
        format_output(result)
    except ValueError as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
