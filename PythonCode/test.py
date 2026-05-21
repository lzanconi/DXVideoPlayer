import tkinter as tk
from tkinter import messagebox # Keep for potential debugging or future re-use, but not called
import json
import socket
import threading
import os
import random
from pathlib import Path

# --- Configuration ---
# DEFAULT_IP = "192.168.0.66"
DEFAULT_IP = "127.0.0.1"
DEFAULT_PORT = 22345
CONNECTION_TIMEOUT = 5 # Timeout for connection attempt

# --- Windows Directory Configuration ---
# Using Path allows compatibility with both relative paths and absolute Windows paths (e.g., r"D:\Videos")
FILE_DIRECTORY = Path("./videos") 

class JSONSenderApp:
    def __init__(self, master):
        self.master = master
        master.title("JSON Command Sender (Windows Server Mode)")
        master.geometry("600x750") 

        # Load file list for FG/BG commands
        self.file_list = self._load_file_list()
        
        # --- Define a fixed, specific list for the Sequence command ---
        self.sequence_file_list = ["sequence1.txt", "sequence2.txt"]

        # Connection State
        self.sock = None 
        self.connected = False 

        ids = [
            ("Movement_1.mp4", 0),
            ("Movement_2.mp4", 1),
            ("Movement_3.mp4", 2),
            ("Movement_4.mp4", 3),
            ("Movement_5.mp4", 4),
            ("Movement_6.mp4", 5)
        ]
        self.ids = dict(ids)

        # 1. Configuration Variables
        self.ip_var = tk.StringVar(value=DEFAULT_IP)
        self.port_var = tk.StringVar(value=str(DEFAULT_PORT))

        # 2. Command-Specific Variables
        default_filename = self.file_list[0] if self.file_list and not self.file_list[0].startswith("No Files Found") else ""
        
        self.filename_fg_var = tk.StringVar(value=default_filename)
        self.fade_in_fg_var = tk.StringVar(value="2.5")
        self.fade_out_fg_var = tk.StringVar(value="3.2")
        self.loop_fg_var = tk.BooleanVar(value=False) 
        
        # Background
        self.filename_bg_var = tk.StringVar(value=default_filename)
        self.fade_in_bg_var = tk.StringVar(value="0.0")
        self.fade_out_bg_var = tk.StringVar(value="0.0")
        self.fg_fade_out_time_var = tk.StringVar(value="2.0")
        self.loop_bg_var = tk.BooleanVar(value=False) 
        
        # Sequence
        default_seq_filename = self.sequence_file_list[0] if self.sequence_file_list else ""
        self.filename_seq_var = tk.StringVar(value=default_seq_filename)
        self.fade_in_seq_var = tk.StringVar(value="1.0")
        self.loop_seq_var = tk.BooleanVar(value=False)

        # Transition
        self.filename_trans_var = tk.StringVar(value=default_filename)
        self.fade_in_trans_var = tk.StringVar(value="0.0")
        self.fade_out_trans_var = tk.StringVar(value="0.0")
        self.trans_fg_fade_out_time_var = tk.StringVar(value="2.0")
        self.loop_trans_var = tk.BooleanVar(value=False) 

        # UI element for transient feedback
        self.feedback_var = tk.StringVar(value="Ready.")

        # --- Setup the UI ---
        self._create_widgets()
        self._update_connection_status()

    def _load_file_list(self):
        """Reads the filenames dynamically using Windows-safe path checks."""
        try:
            if not FILE_DIRECTORY.exists():
                print(f"ERROR: Windows path not found: {FILE_DIRECTORY.resolve()}")
                return ["No Files Found (Directory Missing)"]
                
            file_names = [
                entry.name for entry in FILE_DIRECTORY.iterdir()
                if entry.is_file() and not entry.name.startswith('.')
            ]
            
            if not file_names:
                print(f"Warning: No files found in directory: {FILE_DIRECTORY.resolve()}")
                return ["No Files Found"]
            
            file_names.sort()
            return file_names
            
        except Exception as e:
            print(f"ERROR: Windows error accessing directory: {e}")
            return ["No Files Found (Error)"]

    def _create_filename_dropdown(self, parent_frame, row, col, textvariable, file_source):
        """Creates an OptionMenu with the file list."""
        if file_source and file_source[0].startswith("No Files Found"):
            tk.Entry(parent_frame, textvariable=textvariable, width=30, state='readonly').grid(row=row, column=col, columnspan=3, padx=5, pady=5, sticky="ew")
        else:
            menu = tk.OptionMenu(parent_frame, textvariable, *file_source)
            menu.config(width=28) 
            menu.grid(row=row, column=col, columnspan=3, padx=5, pady=5, sticky="ew")

    def _create_widgets(self):
        # 1. IP and Port Configuration Frame
        config_frame = tk.LabelFrame(self.master, text="Windows Server Target Configuration", padx=10, pady=10)
        config_frame.pack(padx=10, pady=5, fill="x")

        tk.Label(config_frame, text="Target IP:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(config_frame, textvariable=self.ip_var, width=15).grid(row=0, column=1, padx=5, pady=5)
        tk.Label(config_frame, text="Target Port:").grid(row=0, column=2, padx=5, pady=5, sticky="w")
        tk.Entry(config_frame, textvariable=self.port_var, width=8).grid(row=0, column=3, padx=5, pady=5)

        self.status_label = tk.Label(config_frame, text="STATUS: Disconnected", fg="red")
        self.status_label.grid(row=1, column=0, columnspan=2, padx=5, pady=5, sticky="w")
        
        self.connect_button = tk.Button(config_frame, text="Connect", command=self._start_connection_thread, width=12)
        self.connect_button.grid(row=1, column=2, padx=5, pady=5)
        
        self.disconnect_button = tk.Button(config_frame, text="Disconnect", command=self._disconnect, width=12, state=tk.DISABLED)
        self.disconnect_button.grid(row=1, column=3, padx=5, pady=5)

        # 2. Command Buttons Frame
        commands_frame = tk.LabelFrame(self.master, text="JSON Windows Commands", padx=10, pady=5)
        commands_frame.pack(padx=10, pady=5, fill="both", expand=True)

        # --- Command 2: Play Background ---
        bg_frame = tk.LabelFrame(commands_frame, text="Play Background", padx=5, pady=5)
        bg_frame.pack(padx=10, pady=5, fill="x")

        tk.Label(bg_frame, text="Filename:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(bg_frame, 0, 1, self.filename_bg_var, self.file_list) 

        tk.Label(bg_frame, text="Fade In (s):").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(bg_frame, textvariable=self.fade_in_bg_var, width=10).grid(row=1, column=1, padx=5, pady=5, sticky="w")

        tk.Label(bg_frame, text="Fade Out (s):").grid(row=1, column=2, padx=5, pady=5, sticky="w")
        tk.Entry(bg_frame, textvariable=self.fade_out_bg_var, width=10).grid(row=1, column=3, padx=5, pady=5, sticky="w")

        tk.Label(bg_frame, text="FG Fade Out (s):").grid(row=2, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(bg_frame, textvariable=self.fg_fade_out_time_var, width=10).grid(row=2, column=1, padx=5, pady=5, sticky="w") 
        
        tk.Checkbutton(bg_frame, text="Loop", variable=self.loop_bg_var).grid(row=2, column=2, padx=10, pady=5, sticky="w")

        tk.Button(bg_frame, text="Send BG Command", command=self._send_play_background, width=22).grid(row=3, column=0, columnspan=5, pady=5)

        # --- Command 3: TRANSITION TO ---
        trans_frame = tk.LabelFrame(commands_frame, text="TRANSITION TO", padx=5, pady=5)
        trans_frame.pack(padx=10, pady=5, fill="x")

        tk.Label(trans_frame, text="Filename:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(trans_frame, 0, 1, self.filename_trans_var, self.file_list) 

        tk.Label(trans_frame, text="Fade In (s):").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(trans_frame, textvariable=self.fade_in_trans_var, width=10).grid(row=1, column=1, padx=5, pady=5, sticky="w")

        tk.Label(trans_frame, text="Fade Out (s):").grid(row=1, column=2, padx=5, pady=5, sticky="w")
        tk.Entry(trans_frame, textvariable=self.fade_out_trans_var, width=10).grid(row=1, column=3, padx=5, pady=5, sticky="w")

        tk.Label(trans_frame, text="FG Fade Out (s):").grid(row=2, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(trans_frame, textvariable=self.trans_fg_fade_out_time_var, width=10).grid(row=2, column=1, padx=5, pady=5, sticky="w") 
        
        tk.Checkbutton(trans_frame, text="Loop", variable=self.loop_trans_var).grid(row=2, column=2, padx=10, pady=5, sticky="w")

        tk.Button(trans_frame, text="Send Transition Command", command=self._send_transition_to, width=22).grid(row=3, column=0, columnspan=5, pady=5)

        # --- Command 4: Play Sequence ---
        seq_frame = tk.LabelFrame(commands_frame, text="Play Sequence", padx=5, pady=5)
        seq_frame.pack(padx=10, pady=5, fill="x")

        tk.Label(seq_frame, text="Filename:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(seq_frame, 0, 1, self.filename_seq_var, self.sequence_file_list)

        tk.Label(seq_frame, text="Fade In (s):").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(seq_frame, textvariable=self.fade_in_seq_var, width=10).grid(row=1, column=1, padx=5, pady=5, sticky="w")
        
        tk.Checkbutton(seq_frame, text="Loop", variable=self.loop_seq_var).grid(row=1, column=2, padx=10, pady=5, sticky="w")

        tk.Button(seq_frame, text="Send Sequence", command=self._send_play_sequence, width=22).grid(row=2, column=0, columnspan=3, pady=5) 
        
        # 3. Footer/Feedback Bar
        tk.Label(self.master, textvariable=self.feedback_var, relief=tk.SUNKEN, anchor="w").pack(side=tk.BOTTOM, fill=tk.X)

    def _get_target_info(self):
        ip = self.ip_var.get()
        try:
            port = int(self.port_var.get())
            if not (1 <= port <= 65535):
                 raise ValueError()
        except ValueError:
            self._update_feedback("Input Error: Invalid Windows port.", is_error=True)
            return None, None
        return ip, port

    def _update_connection_status(self):
        if self.connected:
            self.status_label.config(text=f"STATUS: Connected to Windows Server ({self.ip_var.get()}:{self.port_var.get()})", fg="green")
            self.connect_button.config(state=tk.DISABLED)
            self.disconnect_button.config(state=tk.NORMAL)
            self._update_feedback("Ready to transmit JSON data.")
        else:
            self.status_label.config(text="STATUS: Disconnected", fg="red")
            self.connect_button.config(state=tk.NORMAL)
            self.disconnect_button.config(state=tk.DISABLED)
            
    def _update_feedback(self, message, is_error=False, duration=5000):
        if is_error:
            self.feedback_var.set(f"ERROR: {message}")
        else:
            self.feedback_var.set(message)
        self.master.after(duration, lambda: self.feedback_var.set("Ready."))

    def _start_connection_thread(self):
        ip, port = self._get_target_info()
        if ip is None or port is None:
            return

        self.status_label.config(text="STATUS: Connecting over Windows Network...", fg="orange")
        self.connect_button.config(state=tk.DISABLED)

        # Setting daemon=True ensures Windows safely kills this background thread if the main GUI closes
        conn_thread = threading.Thread(target=self._connect_target, args=(ip, port))
        conn_thread.daemon = True 
        conn_thread.start()

    def _connect_target(self, ip, port):
        self._disconnect(silent=True) 
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(CONNECTION_TIMEOUT)
            self.sock.connect((ip, port))
            self.connected = True
        except socket.error as e:
            self.connected = False
            self.sock = None
            self.master.after(0, lambda: self._update_feedback(f"Network Connection Failed: {e}", is_error=True))
        except Exception as e:
            self.connected = False
            self.sock = None
            self.master.after(0, lambda: self._update_feedback(f"Unexpected WinError: {e}", is_error=True))
        finally:
            self.master.after(0, self._update_connection_status)

    def _disconnect(self, silent=False):
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR) # Safely clean up buffers on Windows systems
                self.sock.close()
            except socket.error as e:
                if not silent:
                    self._update_feedback(f"Socket Close Warning: {e}")
            finally:
                self.sock = None
                self.connected = False
        self._update_connection_status()

    def _send_json_via_socket(self, message_dict):
        if not self.connected or not self.sock:
            self._update_feedback("Send Failed: Connection lost or missing.", is_error=True)
            return False, ""

        if "No Files Found" in str(message_dict.values()):
             self._update_feedback("Send Failed: Invalid Windows filename selection.", is_error=True)
             return False, ""

        try:
            json_message = json.dumps(message_dict)
            data_to_send = json_message.encode('utf-8')
            self.sock.sendall(data_to_send)
            
            self._update_feedback(f"Sent: {json_message[:60]}...")
            return True, json_message
        except socket.error as e:
            self._disconnect(silent=True) 
            self._update_connection_status()
            self._update_feedback(f"Send Failed: Windows Server reset connection ({e}).", is_error=True)
            return False, ""
        except Exception as e:
            self._update_feedback(f"Send Error: {e}", is_error=True)
            return False, ""

    def _validate_float_input(self, var, field_name):
        try:
            value = float(var.get())
            if value < 0:
                 raise ValueError()
            return value
        except ValueError:
            self._update_feedback(f"Input Error: '{field_name}' must be a non-negative number.", is_error=True)
            return None

    # --- Windows Command Callbacks ---

    def _send_play_background(self):
        filename = self.filename_bg_var.get()
        fade_in = self._validate_float_input(self.fade_in_bg_var, "Background Fade In")
        fade_out = self._validate_float_input(self.fade_out_bg_var, "Background Fade Out")
        fg_fade_out_time = self._validate_float_input(self.fg_fade_out_time_var, "Foreground Fade Out Time") 
        
        if fade_in is None or fade_out is None or fg_fade_out_time is None: return

        # Fallback key safety if custom files populate the dynamic Windows path directory
        assigned_id = self.ids.get(filename, random.randint(0, 100))

        # Windows-friendly forward string path formatting for server-side configurations
        message_dict = {
            # "play_background": "./prod/stresstest/" + filename, 
            "play_background": filename, 
            "loop": self.loop_bg_var.get(), 
            "fade_in_seconds": fade_in, 
            "fg_fade_out_seconds": fg_fade_out_time, 
            "id": assigned_id
        }
        self._send_json_via_socket(message_dict)
    
    def _send_transition_to(self):
        filename = self.filename_trans_var.get()
        fade_in = self._validate_float_input(self.fade_in_trans_var, "Transition Fade In")
        fade_out = self._validate_float_input(self.fade_out_trans_var, "Transition Fade Out")
        fg_fade_out_time = self._validate_float_input(self.trans_fg_fade_out_time_var, "Foreground Fade Out Time") 
        
        if fade_in is None or fade_out is None or fg_fade_out_time is None: return

        message_dict = {
            "transition_to": filename,
            "fade_in_seconds": fade_in,
            "fg_fade_out_time": fg_fade_out_time,
            "loop": self.loop_trans_var.get()
        }
        self._send_json_via_socket(message_dict)

    def _send_play_sequence(self):
        filename = self.filename_seq_var.get()
        fade_in = self._validate_float_input(self.fade_in_seq_var, "Sequence Fade In")
        if fade_in is None: return

        message_dict = {
            "play_sequence": filename,
            "fade_in_seconds": fade_in, 
            "loop": self.loop_seq_var.get() 
        }
        self._send_json_via_socket(message_dict)


if __name__ == '__main__':
    root = tk.Tk()
    app = JSONSenderApp(root)
    # Ensure standard socket cleanup on standard window close events (Alt+F4 / Close Button)
    root.protocol("WM_DELETE_WINDOW", lambda: (app._disconnect(silent=True), root.destroy()))
    root.mainloop()