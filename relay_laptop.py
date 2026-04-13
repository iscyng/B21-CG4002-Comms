import paho.mqtt.client as mqtt
import json
import threading
import time
import sys
import zlib
from datetime import datetime
from cryptography.fernet import Fernet

# --- LIGHTWEIGHT IOT ENCRYPTION (For ESP32) ---
def xor_cipher(data, key=b"NusEngineering"):
    if isinstance(data, str):
        data = data.encode('utf-8')
    key_bytes = key * (len(data) // len(key) + 1)
    return bytes(a ^ b for a, b in zip(data, key_bytes))

# --- AES ENCRYPTION KEY ---
SECRET_KEY = b'uB_yC7L9V3b_j-fV0v-aP7lK1U2hX7T_w_N4K8H_JIg='
cipher = Fernet(SECRET_KEY)

# --- Colors ---
COLOR_LAPTOP = "\033[94m"  # Blue
COLOR_ESP32 = "\033[92m"   # Green
COLOR_SPEED = "\033[93m"   # Yellow
RESET = "\033[0m"          # Reset

broker_ip = "XXXXXX" # <--- IP Addr of EC2 Instance
port = 1883

# Variables for calculating kbps and print timing
bytes_received = 0
last_print_time = 0

# --- Hex/Human-Readable Logging ---
def log_packet_to_file(topic, raw_payload, parsed_data):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    hex_raw = raw_payload.hex().upper()
    hex_formatted = ' '.join(hex_raw[i:i+2] for i in range(0, len(hex_raw), 2))
    
    seq = parsed_data.get("sequence", "N/A")
    pkt_type = parsed_data.get("type", "N/A")
    
    if "payload" in parsed_data:
        p = parsed_data["payload"]
        readings = f"Accel(x:{p.get('ax')}, y:{p.get('ay')}, z:{p.get('az')})"
    else:
        readings = "N/A"
        
    crc_val = zlib.crc32(raw_payload) & 0xFFFFFFFF
    crc_hex = f"0x{crc_val:08X}"

    with open("communications_log.txt", "a") as log_file:
        log_file.write(f"[{timestamp}] Topic: {topic}\n")
        log_file.write(f"  RAW (HEX) : {hex_formatted}\n")
        log_file.write(f"  PARSED    : Seq={seq} | Type={pkt_type} | Data=[{readings}] | CRC={crc_hex}\n")
        log_file.write("-" * 75 + "\n")

# --- MQTT Callbacks ---
def on_connect(client, userdata, flags, rc):
    print(f"{COLOR_LAPTOP}[LAPTOP] Connected to Local Broker!{RESET}")
    client.subscribe("esp32/to/laptop")
    client.subscribe("wand/imu/raw")
    client.subscribe("ultra96/#")
    
    # --- NEW: Subscribe to Unity's Action Topic ---
    client.subscribe("visualizer/to/wand")

def on_message(client, userdata, msg):
    global bytes_received, last_print_time
    
    bytes_received += len(msg.payload) + len(msg.topic)

    try:
        # 1. ALWAYS CATCH AI PREDICTIONS IMMEDIATELY
        if msg.topic.startswith("ultra96/"):
            if msg.topic == "ultra96/to/laptop":
                # Handle Haptic Command (Fallback if Ultra96 still sends them)
                decrypted_bytes = cipher.decrypt(msg.payload)
                data = json.loads(decrypted_bytes.decode('utf-8'))
                if data.get("command") == "vibrate":
                    encrypted_cmd = xor_cipher(json.dumps({"action": "vibrate"}))
                    client.publish("wand/cmd", encrypted_cmd)
                    print(f"\n\033[93m[RELAY] Forwarded Vibrate Command from AI to Glove! 📳\033[0m")
            else:
                # Handle Prediction Display
                result = msg.payload.decode('utf-8')
                print(f"\n\033[95m[ULTRA96 AI PREDICTION] 🧠 {result}\033[0m")
            return 

        # --- NEW: Catch Vibrate Command from Unity Visualizer ---
        elif msg.topic == "visualizer/to/wand":
            data = json.loads(msg.payload.decode('utf-8'))
            
            # Check for "action" key based on C# WandActionMessage
            if data.get("action") == "vibrate": 
                cmd_json = json.dumps({"action": "vibrate"})
                encrypted_cmd = xor_cipher(cmd_json)
                client.publish("wand/cmd", encrypted_cmd)
                print(f"\n\033[93m[GAME EVENT] Target Popped! Forwarding Vibrate to Glove! 📳\033[0m")
            return

        # 2. HANDLE SENSOR DATA (ESP32 -> Ultra96)
        if msg.topic == "esp32/to/laptop":
            decrypted_bytes = xor_cipher(msg.payload)
            data = json.loads(decrypted_bytes.decode('utf-8'))
        else:
            data = json.loads(msg.payload.decode())

        # 3. FORWARD TO ULTRA96 AT FULL SPEED (No delay here)
        log_packet_to_file(msg.topic, msg.payload, data)
        json_string = json.dumps(data)
        encrypted_payload = cipher.encrypt(json_string.encode())
        client.publish("laptop/to/ultra96", encrypted_payload)

        # 4. RATE-LIMITED PRINTING (Once per second)
        current_time = time.time()
        if current_time - last_print_time >= 1.0:
            p = data.get("payload", {})
            sys.stdout.write(f"\r\033[K{COLOR_ESP32}[LIVE BNO055] ax:{p.get('ax', 0.0):.2f} | ay:{p.get('ay', 0.0):.2f} | az:{p.get('az', 0.0):.2f}{RESET}")
            sys.stdout.flush()
            last_print_time = current_time
            
    except Exception as e:
        pass # Silently handle parsing errors to keep the stream moving

# --- Speed Monitor ---
def speed_monitor():
    global bytes_received
    is_connected = True 
    while True:
        # --- NEW: Check every 2 seconds to absorb cloud network lag ---
        time.sleep(2.0)
        kbps = (bytes_received * 8) / 2000.0 
        
        if kbps == 0.0 and is_connected:
            print(f"\n\033[91m[ALERT] FireBeetle Disconnected!\033[0m")
            is_connected = False
        elif kbps > 0.0 and not is_connected:
            print(f"\n\033[92m[ALERT] FireBeetle Reconnected!\033[0m")
            is_connected = True
        
        # This writes to the very bottom line
        sys.stdout.write(f"\n\033[K{COLOR_SPEED}--- Current Speed: {kbps:.2f} kbps ---{RESET}\033[F")
        sys.stdout.flush()
        bytes_received = 0

if __name__ == "__main__":
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"Connecting to broker at {broker_ip}...")
    client.connect(broker_ip, port, 60)

    threading.Thread(target=speed_monitor, daemon=True).start()

    print(f"{COLOR_LAPTOP}--- LIVE SENSOR STREAM ACTIVE (50Hz Forwarding, 1Hz Display) ---{RESET}")
    client.loop_forever()

