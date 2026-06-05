import tkinter as tk
from tkinter import messagebox, scrolledtext
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
FILE_DIRECTORY = Path("./videos") 

class JSONSenderApp:
    def __init__(self, master):
        self.master = master
        master.title("JSON Command Sender (Two-Way Windows Mode)")
        master.geometry("600x850") # Set a comfortable initial window height

        # Load file list for FG/BG commands
        self.file_list = self._load_file_list()
        self.sequence_file_list = ["sequence1.txt", "sequence2.txt"]

        # Connection State
        self.sock = None
        self.connected = False

        ids = [
            ("Movement_1.mp4", 0), ("Movement_2.mp4", 1), ("Movement_3.mp4", 2),
            ("Movement_4.mp4", 3), ("Movement_5.mp4", 4), ("Movement_6.mp4", 5)
        ]
        self.ids = dict(ids)

        # 1. Configuration Variables
        self.ip_var = tk.StringVar(value=DEFAULT_IP)
        self.port_var = tk.StringVar(value=str(DEFAULT_PORT))

        # 2. Command-Specific Variables
        default_filename = self.file_list[0] if self.file_list and not self.file_list[0].startswith("No Files Found") else ""
        
        self.filename_bg_var = tk.StringVar(value=default_filename)
        self.fade_in_bg_var = tk.StringVar(value="0.0")
        self.fade_out_bg_var = tk.StringVar(value="0.0")
        self.fg_fade_out_time_var = tk.StringVar(value="2.0")
        self.loop_bg_var = tk.BooleanVar(value=False)

        self.filename_fg_var = tk.StringVar(value=default_filename)
        self.fade_in_fg_var = tk.StringVar(value="0.0")
        self.fade_out_fg_var = tk.StringVar(value="0.0")
        self.loop_fg_var = tk.BooleanVar(value=False)
        
        self.filename_trans_var = tk.StringVar(value=default_filename)
        self.fade_in_trans_var = tk.StringVar(value="0.0")
        self.fade_out_trans_var = tk.StringVar(value="0.0")
        self.trans_fg_fade_out_time_var = tk.StringVar(value="2.0")
        self.loop_trans_var = tk.BooleanVar(value=False) 

        self.filename_seq_var = tk.StringVar(value=self.sequence_file_list[0] if self.sequence_file_list else "")
        self.fade_in_seq_var = tk.StringVar(value="1.0")
        self.fade_out_seq_var = tk.StringVar(value="1.0")
        self.loop_seq_var = tk.BooleanVar(value=False)

        # Cover Command Variables
        self.filename_cover_var = tk.StringVar(value=default_filename)
        self.fade_in_cover_var = tk.StringVar(value="0.0")
        self.fade_out_cover_var = tk.StringVar(value="0.0")
        self.loop_cover_var = tk.BooleanVar(value=False)

        # --- Transition To (New Structure) Variables ---
        self.filename_trans_new_var = tk.StringVar(value=default_filename)
        self.foreground_trans_var = tk.StringVar(value=default_filename)
        self.background_trans_var = tk.StringVar(value=default_filename)
        self.fade_in_trans_new_var = tk.StringVar(value="0.0")
        self.loop_trans_new_var = tk.BooleanVar(value=False)

        # --- Checkbox States for Optional Elements ---
        self.include_fg_var = tk.BooleanVar(value=True)
        self.include_bg_var = tk.BooleanVar(value=True)

        self.feedback_var = tk.StringVar(value="Ready.")

        # --- Setup the UI ---
        self._create_widgets()
        self._update_connection_status()

    def _load_file_list(self):
        try:
            if not FILE_DIRECTORY.exists():
                return ["No Files Found (Directory Missing)"]
            file_names = [entry.name for entry in FILE_DIRECTORY.iterdir() if entry.is_file() and not entry.name.startswith('.')]
            if not file_names: return ["No Files Found"]
            file_names.sort()
            return file_names
        except Exception:
            return ["No Files Found (Error)"]

    def _create_filename_dropdown(self, parent_frame, row, col, textvariable, file_source, columnspan=3):
        if file_source and file_source[0].startswith("No Files Found"):
            tk.Entry(parent_frame, textvariable=textvariable, width=30, state='readonly').grid(row=row, column=col, columnspan=columnspan, padx=5, pady=5, sticky="ew")
        else:
            menu = tk.OptionMenu(parent_frame, textvariable, *file_source)
            menu.config(width=25 if columnspan==2 else 28)
            menu.grid(row=row, column=col, columnspan=columnspan, padx=5, pady=5, sticky="ew")

    def _create_widgets(self):
        # Configuration Frame
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

        # Create a container frame that handles scrolling for the long commands list
        scroll_container = tk.Frame(self.master)
        scroll_container.pack(padx=10, pady=5, fill="both", expand=True)

        canvas = tk.Canvas(scroll_container, borderwidth=0, highlightthickness=0)
        scrollbar = tk.Scrollbar(scroll_container, orient="vertical", command=canvas.yview)
        
        # The scrollable interior panel where all sub-frames will be packed
        commands_frame = tk.LabelFrame(canvas, text="JSON Commands", padx=10, pady=5)
        
        # Configure canvas window behavior
        canvas.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        
        # Create a window inside the canvas to hold our commands layout
        canvas_window = canvas.create_window((0, 0), window=commands_frame, anchor="nw")

        # Dynamically link frame boundaries back to the scrollable scroll-region of the canvas
        def _on_frame_configure(event):
            canvas.configure(scrollregion=canvas.bbox("all"))
        commands_frame.bind("<Configure>", _on_frame_configure)

        # Auto-adjust the inner panel width to seamlessly match the size of the canvas window
        def _on_canvas_configure(event):
            canvas.itemconfig(canvas_window, width=event.width)
        canvas.bind("<Configure>", _on_canvas_configure)

        # Enable mousewheel scrolling on hover inside the panel region
        def _on_mousewheel(event):
            canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        canvas.bind_all("<MouseWheel>", _on_mousewheel)

        # Stop
        stop_frame = tk.LabelFrame(commands_frame, text="Stop", padx=5, pady=5)
        stop_frame.pack(padx=10, pady=5, fill="x")
        tk.Button(stop_frame, text="Stop", command=self._send_stop, width=22).grid(row=0, column=0, columnspan=5, pady=5)

        # Play Background
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

        # Play Foreground
        fg_frame = tk.LabelFrame(commands_frame, text="Play Foreground", padx=5, pady=5)
        fg_frame.pack(padx=10, pady=5, fill="x")
        tk.Label(fg_frame, text="Filename:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(fg_frame, 0, 1, self.filename_fg_var, self.file_list)
        tk.Label(fg_frame, text="Fade In (s):").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(fg_frame, textvariable=self.fade_in_fg_var, width=10).grid(row=1, column=1, padx=5, pady=5, sticky="w")
        tk.Label(fg_frame, text="Fade Out (s):").grid(row=1, column=2, padx=5, pady=5, sticky="w")
        tk.Entry(fg_frame, textvariable=self.fade_out_fg_var, width=10).grid(row=1, column=3, padx=5, pady=5, sticky="w")
        tk.Checkbutton(fg_frame, text="Loop", variable=self.loop_fg_var).grid(row=2, column=0, padx=10, pady=5, sticky="w")
        tk.Button(fg_frame, text="Send FG Command", command=self._send_play_foreground, width=18).grid(row=2, column=1, columnspan=3, pady=10)

        # Play Cover
        cover_frame = tk.LabelFrame(commands_frame, text="Play Cover", padx=5, pady=5)
        cover_frame.pack(padx=10, pady=5, fill="x")
        tk.Label(cover_frame, text="Filename:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(cover_frame, 0, 1, self.filename_cover_var, self.file_list) 
        tk.Label(cover_frame, text="Fade In (s):").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(cover_frame, textvariable=self.fade_in_cover_var, width=10).grid(row=1, column=1, padx=5, pady=5, sticky="w")
        tk.Label(cover_frame, text="Fade Out (s):").grid(row=1, column=2, padx=5, pady=5, sticky="w")
        tk.Entry(cover_frame, textvariable=self.fade_out_cover_var, width=10).grid(row=1, column=3, padx=5, pady=5, sticky="w")
        tk.Checkbutton(cover_frame, text="Loop", variable=self.loop_cover_var).grid(row=2, column=0, padx=10, pady=5, sticky="w")
        tk.Button(cover_frame, text="Send Cover Command", command=self._send_play_cover, width=18).grid(row=2, column=1, columnspan=3, pady=10)
        tk.Button(cover_frame, text="Hide Cover", command=self._send_hide_cover, width=18, bg="#FFCCCC").grid(row=3, column=1, columnspan=3, pady=5)

        # Transition To
        trans_new_frame = tk.LabelFrame(commands_frame, text="Transition To", padx=5, pady=5)
        trans_new_frame.pack(padx=10, pady=5, fill="x")
        
        # Row 0: Filename
        tk.Label(trans_new_frame, text="Filename:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(trans_new_frame, 0, 1, self.filename_trans_new_var, self.file_list, columnspan=2)
        
        # Row 1: Foreground List + Activation Checkbox
        tk.Label(trans_new_frame, text="Foreground:").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(trans_new_frame, 1, 1, self.foreground_trans_var, self.file_list, columnspan=2)
        tk.Checkbutton(trans_new_frame, text="Include", variable=self.include_fg_var).grid(row=1, column=3, padx=5, pady=5, sticky="w")

        # Row 2: Background List + Activation Checkbox
        tk.Label(trans_new_frame, text="Background:").grid(row=2, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(trans_new_frame, 2, 1, self.background_trans_var, self.file_list, columnspan=2)
        tk.Checkbutton(trans_new_frame, text="Include", variable=self.include_bg_var).grid(row=2, column=3, padx=5, pady=5, sticky="w")

        # Row 3: Durations
        tk.Label(trans_new_frame, text="Fade In (s):").grid(row=3, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(trans_new_frame, textvariable=self.fade_in_trans_new_var, width=10).grid(row=3, column=1, padx=5, pady=5, sticky="w")
        tk.Checkbutton(trans_new_frame, text="Loop", variable=self.loop_trans_new_var).grid(row=3, column=2, padx=10, pady=5, sticky="w")
        
        # Row 4: Action Button
        tk.Button(trans_new_frame, text="Send Transition Command", command=self._send_transition_to_new, width=22).grid(row=4, column=0, columnspan=4, pady=10)

        # Play Sequence
        seq_frame = tk.LabelFrame(commands_frame, text="Play Sequence", padx=5, pady=5)
        seq_frame.pack(padx=10, pady=5, fill="x")
        tk.Label(seq_frame, text="Filename:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self._create_filename_dropdown(seq_frame, 0, 1, self.filename_seq_var, self.file_list)
        tk.Label(seq_frame, text="Fade In (s):").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        tk.Entry(seq_frame, textvariable=self.fade_in_seq_var, width=10).grid(row=1, column=1, padx=5, pady=5, sticky="w")
        tk.Label(seq_frame, text="Fade Out (s):").grid(row=1, column=2, padx=5, pady=5, sticky="w")
        tk.Entry(seq_frame, textvariable=self.fade_out_seq_var, width=10).grid(row=1, column=3, padx=5, pady=5, sticky="w")
        tk.Checkbutton(seq_frame, text="Loop", variable=self.loop_seq_var).grid(row=2, column=0, padx=10, pady=5, sticky="w")
        tk.Button(seq_frame, text="Send Sequence", command=self._send_play_sequence, width=22).grid(row=2, column=1, columnspan=3, pady=10)

        # Terminal display box for incoming server messages
        terminal_frame = tk.LabelFrame(self.master, text="Server Responses (Incoming Data)", padx=10, pady=5)
        terminal_frame.pack(padx=10, pady=5, fill="x")
        
        self.terminal = scrolledtext.ScrolledText(terminal_frame, wrap=tk.WORD, height=6, bg="black", fg="#00FF00", font=("Consolas", 10))
        self.terminal.pack(fill="both", expand=True)
        self.terminal.config(state=tk.DISABLED) 
        
        # Status Bar
        tk.Label(self.master, textvariable=self.feedback_var, relief=tk.SUNKEN, anchor="w").pack(side=tk.BOTTOM, fill=tk.X)

    def _log_to_terminal(self, text):
        self.terminal.config(state=tk.NORMAL)
        self.terminal.insert(tk.END, text + "\n")
        self.terminal.see(tk.END) 
        self.terminal.config(state=tk.DISABLED)

    def _get_target_info(self):
        try:
            port = int(self.port_var.get())
            if not (1 <= port <= 65535): raise ValueError()
            return self.ip_var.get(), port
        except ValueError:
            self._update_feedback("Input Error: Invalid port.", is_error=True)
            return None, None

    def _update_connection_status(self):
        if self.connected:
            self.status_label.config(text=f"STATUS: Connected to {self.ip_var.get()}:{self.port_var.get()}", fg="green")
            self.connect_button.config(state=tk.DISABLED)
            self.disconnect_button.config(state=tk.NORMAL)
        else:
            self.status_label.config(text="STATUS: Disconnected", fg="red")
            self.connect_button.config(state=tk.NORMAL)
            self.disconnect_button.config(state=tk.DISABLED)
            
    def _update_feedback(self, message, is_error=False, duration=5000):
        self.feedback_var.set(f"ERROR: {message}" if is_error else message)
        self.master.after(duration, lambda: self.feedback_var.set("Ready."))

    def _start_connection_thread(self):
        ip, port = self._get_target_info()
        if ip is None or port is None: return
        self.status_label.config(text="STATUS: Connecting...", fg="orange")
        self.connect_button.config(state=tk.DISABLED)

        conn_thread = threading.Thread(target=self._connect_target, args=(ip, port))
        conn_thread.daemon = True 
        conn_thread.start()

    def _connect_target(self, ip, port):
        self._disconnect(silent=True) 
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(CONNECTION_TIMEOUT)
            self.sock.connect((ip, port))
            self.sock.settimeout(None) 
            self.connected = True
            
            receiver_thread = threading.Thread(target=self._listen_for_server_responses)
            receiver_thread.daemon = True
            receiver_thread.start()

            print("[CONSOLE LOG]: Connected to server. Listening for incoming data...")
            self.master.after(0, lambda: self._log_to_terminal("[SYSTEM]: Connected to server. Listening for pulses..."))
        except socket.error as e:
            self.connected = False
            self.sock = None
            self.master.after(0, lambda: self._update_feedback(f"Connection Failed: {e}", is_error=True))
        finally:
            self.master.after(0, self._update_connection_status)

    def _listen_for_server_responses(self):
        while self.connected and self.sock:
            try:
                data = self.sock.recv(4096)
                if not data:
                    break
                decoded_msg = data.decode('utf-8')
                print(f"[CONSOLE LOG] Received: {decoded_msg}")
                self.master.after(0, lambda msg=decoded_msg: self._log_to_terminal(f"[SERVER]: {msg}"))
            except socket.error:
                break
            except Exception as e:
                print(f"[CONSOLE ERROR]: {e}")
                self.master.after(0, lambda err=e: self._log_to_terminal(f"[SYSTEM ERROR]: {err}"))
                break

        print("[CONSOLE LOG]: Connection severed or closed by host server.")
        self.master.after(0, lambda: self._log_to_terminal("[SYSTEM]: Connection severed or dropped by host server."))
        self.master.after(0, self._disconnect)

    def _disconnect(self, silent=False):
        if self.sock:
            try:
                self.connected = False 
                self.sock.shutdown(socket.SHUT_RDWR)
                self.sock.close()
            except socket.error:
                pass
            finally:
                self.sock = None
        self._update_connection_status()

    def _send_json_via_socket(self, message_dict):
        if not self.connected or not self.sock:
            self._update_feedback("Send Failed: Not connected.", is_error=True)
            return False

        try:
            json_message = json.dumps(message_dict)
            self.sock.sendall(json_message.encode('utf-8'))
            self._update_feedback("Data package sent successfully.")
            return True
        except socket.error as e:
            self._disconnect(silent=True)
            self._update_feedback(f"Transmission Failed: Link lost ({e})", is_error=True)
            return False

    def _validate_float_input(self, var, field_name):
        try:
            value = float(var.get())
            if value < 0: raise ValueError()
            return value
        except ValueError:
            self._update_feedback(f"Input Error: '{field_name}' must be non-negative.", is_error=True)
            return None
        
    def _send_stop(self):
        message_dict = {"stop": ""}
        self._send_json_via_socket(message_dict)

    def _send_play_background(self):
        filename = self.filename_bg_var.get()
        fade_in = self._validate_float_input(self.fade_in_bg_var, "Background Fade In")
        fade_out = self._validate_float_input(self.fade_out_bg_var, "Background Fade Out")
        fg_fade_out_time = self._validate_float_input(self.fg_fade_out_time_var, "Foreground Fade Out Time") 
        if fade_in is None or fade_out is None or fg_fade_out_time is None: return

        message_dict = {
            "play_background": filename, 
            "loop": self.loop_bg_var.get(), 
            "fade_in_seconds": fade_in, 
            "fade_out_seconds": fade_out, 
            "id": self.ids.get(filename, random.randint(0, 100))
        }
        self._send_json_via_socket(message_dict)

    def _send_play_foreground(self):
        filename = self.filename_fg_var.get()
        fade_in = self._validate_float_input(self.fade_in_fg_var, "Foreground Fade In")
        fade_out = self._validate_float_input(self.fade_out_fg_var, "Foreground Fade Out")
        if fade_in is None or fade_out is None: return
        
        message_dict = {
            "play_foreground": filename,
            "fade_in_seconds": fade_in,
            "fade_out_seconds": fade_out,
            "loop": self.loop_fg_var.get()
        }
        self._send_json_via_socket(message_dict)

    def _send_play_cover(self):
        filename = self.filename_cover_var.get()
        fade_in = self._validate_float_input(self.fade_in_cover_var, "Cover Fade In")
        fade_out = self._validate_float_input(self.fade_out_cover_var, "Cover Fade Out")
        if fade_in is None or fade_out is None: return
        
        message_dict = {
            "play_cover": filename,
            "fade_in_seconds": fade_in,
            "fade_out_seconds": fade_out,
            "loop": self.loop_cover_var.get()
        }
        self._send_json_via_socket(message_dict)
    
    def _send_hide_cover(self):
        message_dict = {"hide_cover": ""}
        self._send_json_via_socket(message_dict)

    def _send_transition_to_new(self):
        print("DEBUG: Preparing to send Transition To command...")
        filename = self.filename_trans_new_var.get()
        
        # Read dropdowns or assign empty strings depending on the checkboxes
        foreground = self.foreground_trans_var.get() if self.include_fg_var.get() else ""
        background = self.background_trans_var.get() if self.include_bg_var.get() else ""
        
        fade_in = self._validate_float_input(self.fade_in_trans_new_var, "Transition Fade In")
        if fade_in is None: return

        message_dict = {
            "transition_to": filename,
            "foreground": foreground,
            "background": background,
            "fade_in_seconds": fade_in,
            "loop": self.loop_trans_new_var.get()
        }
        self._send_json_via_socket(message_dict)

    def _send_play_sequence(self):
        filename = self.filename_seq_var.get()
        fade_in = self._validate_float_input(self.fade_in_seq_var, "Sequence Fade In")
        fade_out = self._validate_float_input(self.fade_out_seq_var, "Sequence Fade Out")
        if fade_in is None or fade_out is None: return

        message_dict = {
            "play_sequence": filename, "fade_in_seconds": fade_in, "fade_out_seconds": fade_out, "loop": self.loop_seq_var.get() 
        }
        self._send_json_via_socket(message_dict)

if __name__ == '__main__':
    root = tk.Tk()
    app = JSONSenderApp(root)
    root.protocol("WM_DELETE_WINDOW", lambda: (app._disconnect(silent=True), root.destroy()))
    root.mainloop()