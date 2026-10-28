"""Configuration form component using CustomTkinter."""

from pathlib import Path
from threading import Thread
from tkinter import filedialog
from urllib.parse import urlsplit

import customtkinter as ctk
import httpx

from ...core import TranslationConfig
from ...translator import PROVIDER_PRESETS

# Language display name -> language code (must match languages.py)
LANGUAGES = {
    "English": "en",
    "Chinese": "zh-Hans",
    "Spanish": "es",
    "French": "fr",
    "German": "de",
    "Italian": "it",
}

LANG_NAMES = list(LANGUAGES.keys())
CUSTOM_PROVIDER = "custom (OpenAI-compatible)"
CUSTOM_DEFAULT_BASE = "https://1a1api.top/v1"


def normalize_api_base(value: str) -> str:
    """Validate an OpenAI-compatible base URL without its final endpoint path."""
    value = value.strip().rstrip("/")
    if not value:
        raise ValueError("Please enter an API Base URL")
    url = urlsplit(value)
    # HTTPS protects API keys on public networks; loopback HTTP supports dev gateways.
    if url.scheme != "https" and not (
        url.scheme == "http" and url.hostname in ("localhost", "127.0.0.1", "::1")
    ):
        raise ValueError("API Base URL must use HTTPS (HTTP only for localhost)")
    if not url.netloc or url.username or url.password or url.query or url.fragment:
        raise ValueError("Invalid API Base URL")
    if url.path.lower().endswith("/chat/completions"):
        raise ValueError("Enter the base URL (e.g. /v1), not /chat/completions")
    return value


class ConfigForm(ctk.CTkFrame):
    """Configuration form for translation settings."""

    def __init__(self, master):
        """Initialize configuration form."""
        super().__init__(master, corner_radius=10)

        inner = ctk.CTkFrame(self, fg_color="transparent")
        inner.pack(fill="x", padx=14, pady=10)

        # --- Row 1: ROM File ---
        ctk.CTkLabel(inner, text="ROM File", font=("", 12, "bold")).pack(anchor="w")
        rom_row = ctk.CTkFrame(inner, fg_color="transparent")
        rom_row.pack(fill="x", pady=(2, 8))
        self.rom_entry = ctk.CTkEntry(
            rom_row, placeholder_text="Select a GBA ROM file...", height=30
        )
        self.rom_entry.pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            rom_row, text="Browse", width=80, height=30, command=self._browse_rom
        ).pack(side="right")

        # --- Row 1.5: Output Directory ---
        ctk.CTkLabel(inner, text="Output Directory", font=("", 12, "bold")).pack(anchor="w")
        output_row = ctk.CTkFrame(inner, fg_color="transparent")
        output_row.pack(fill="x", pady=(2, 8))
        self.output_entry = ctk.CTkEntry(
            output_row, placeholder_text="Select output directory...", height=30
        )
        self.output_entry.pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            output_row, text="Browse", width=80, height=30, command=self._browse_output
        ).pack(side="right")

        # --- Row 2: Languages ---
        ctk.CTkLabel(inner, text="Languages", font=("", 12, "bold")).pack(anchor="w")
        lang_row = ctk.CTkFrame(inner, fg_color="transparent")
        lang_row.pack(fill="x", pady=(2, 8))

        src_frame = ctk.CTkFrame(lang_row, fg_color="transparent")
        src_frame.pack(side="left", fill="x", expand=True, padx=(0, 6))
        ctk.CTkLabel(src_frame, text="Source:", font=("", 11)).pack(anchor="w")
        self.source_lang = ctk.CTkComboBox(src_frame, values=LANG_NAMES, state="readonly", height=30)
        self.source_lang.set("English")
        self.source_lang.pack(fill="x", pady=(2, 0))

        tgt_frame = ctk.CTkFrame(lang_row, fg_color="transparent")
        tgt_frame.pack(side="right", fill="x", expand=True, padx=(6, 0))
        ctk.CTkLabel(tgt_frame, text="Target:", font=("", 11)).pack(anchor="w")
        self.target_lang = ctk.CTkComboBox(tgt_frame, values=LANG_NAMES, state="readonly", height=30)
        self.target_lang.set("Chinese")
        self.target_lang.pack(fill="x", pady=(2, 0))

        # --- Row 3: Provider + Model ---
        ctk.CTkLabel(inner, text="LLM API", font=("", 12, "bold")).pack(anchor="w")
        pm_row = ctk.CTkFrame(inner, fg_color="transparent")
        pm_row.pack(fill="x", pady=(2, 4))

        prov_frame = ctk.CTkFrame(pm_row, fg_color="transparent")
        prov_frame.pack(side="left", fill="x", expand=True, padx=(0, 6))
        ctk.CTkLabel(prov_frame, text="Provider:", font=("", 11)).pack(anchor="w")
        self.provider = ctk.CTkComboBox(
            prov_frame, values=list(PROVIDER_PRESETS.keys()) + [CUSTOM_PROVIDER],
            state="readonly", height=30, command=self._on_provider_change
        )
        self.provider.set("deepseek")
        self.provider.pack(fill="x", pady=(2, 0))

        model_frame = ctk.CTkFrame(pm_row, fg_color="transparent")
        model_frame.pack(side="right", fill="x", expand=True, padx=(6, 0))
        ctk.CTkLabel(model_frame, text="Model:", font=("", 11)).pack(anchor="w")
        self.model_entry = ctk.CTkEntry(model_frame, height=30)
        self.model_entry.insert(0, PROVIDER_PRESETS["deepseek"][1])
        self.model_entry.pack(fill="x", pady=(2, 0))

        # --- Row 4: API Key ---
        ctk.CTkLabel(inner, text="API Key:", font=("", 11)).pack(anchor="w", pady=(4, 0))
        self.api_key_entry = ctk.CTkEntry(
            inner, placeholder_text="sk-xxxxxxxxxxxxxxxxxxxxxxxx", height=30, show="*"
        )
        self.api_key_entry.pack(fill="x", pady=(2, 0))

        # --- Custom OpenAI-compatible endpoint ---
        ctk.CTkLabel(inner, text="API Base URL:", font=("", 11)).pack(
            anchor="w", pady=(8, 0)
        )
        self.api_base_entry = ctk.CTkEntry(
            inner,
            placeholder_text="e.g. https://1a1api.top/v1 (no /chat/completions)",
            height=30,
        )
        self.api_base_entry.insert(0, PROVIDER_PRESETS["deepseek"][0])
        self.api_base_entry.pack(fill="x", pady=(2, 0))

        test_row = ctk.CTkFrame(inner, fg_color="transparent")
        test_row.pack(fill="x", pady=(5, 0))
        self.test_api_button = ctk.CTkButton(
            test_row,
            text="Test API Connection (small charge possible)",
            command=self._test_api_connection,
            height=28,
        )
        self.test_api_button.pack(side="left")
        self.api_test_status = ctk.CTkLabel(
            inner, text="", font=("", 11), wraplength=700, justify="left"
        )
        self.api_test_status.pack(anchor="w", pady=(2, 0))

        # --- Advanced (collapsible) ---
        self.advanced_visible = False
        self.advanced_button = ctk.CTkButton(
            inner, text="+ Advanced", command=self._toggle_advanced,
            fg_color="transparent", text_color=("gray40", "gray60"),
            hover_color=("gray85", "gray25"), height=24, font=("", 11),
        )
        self.advanced_button.pack(anchor="w", pady=(6, 0))

        self.advanced_frame = ctk.CTkFrame(inner, fg_color="transparent")
        adv_row = ctk.CTkFrame(self.advanced_frame, fg_color="transparent")
        adv_row.pack(fill="x", pady=(4, 0))

        bf = ctk.CTkFrame(adv_row, fg_color="transparent")
        bf.pack(side="left", fill="x", expand=True, padx=(0, 6))
        ctk.CTkLabel(bf, text="Batch Size:", font=("", 11)).pack(anchor="w")
        self.batch_size = ctk.CTkEntry(bf, height=30)
        self.batch_size.insert(0, "30")
        self.batch_size.pack(fill="x", pady=(2, 0))

        wf = ctk.CTkFrame(adv_row, fg_color="transparent")
        wf.pack(side="right", fill="x", expand=True, padx=(6, 0))
        ctk.CTkLabel(wf, text="Max Workers:", font=("", 11)).pack(anchor="w")
        self.max_workers = ctk.CTkEntry(wf, height=30)
        self.max_workers.insert(0, "10")
        self.max_workers.pack(fill="x", pady=(2, 0))

    def _on_provider_change(self, provider_name: str):
        """Load a preset URL/model, or choose an editable custom endpoint."""
        preset = PROVIDER_PRESETS.get(provider_name)
        self.model_entry.delete(0, "end")
        self.api_base_entry.delete(0, "end")
        if preset:
            self.model_entry.insert(0, preset[1])
            self.api_base_entry.insert(0, preset[0])
        elif provider_name == CUSTOM_PROVIDER:
            self.api_base_entry.insert(0, CUSTOM_DEFAULT_BASE)
        # Avoid accidentally sending a previous provider's key to a new host.
        self.api_key_entry.delete(0, "end")
        self.api_test_status.configure(text="")

    def _get_api_settings(self) -> tuple[str, str, str]:
        """Return base URL, model ID, and key without checking the ROM."""
        base_url = normalize_api_base(self.api_base_entry.get())
        model = self.model_entry.get().strip()
        api_key = self.api_key_entry.get().strip()
        if not model:
            raise ValueError("Please enter the exact model ID from your provider")
        if not api_key:
            raise ValueError("Please enter your API key")
        return base_url, model, api_key

    def _test_api_connection(self):
        """Check compatibility without uploading a ROM (one minimal paid request)."""
        try:
            base_url, model, api_key = self._get_api_settings()
        except ValueError as exc:
            self.api_test_status.configure(text=str(exc), text_color="#f87171")
            return

        self.test_api_button.configure(state="disabled")
        self.api_test_status.configure(text="Testing chat/completions...", text_color="gray")

        def worker():
            try:
                response = httpx.post(
                    f"{base_url}/chat/completions",
                    headers={
                        "Authorization": f"Bearer {api_key}",
                        "Content-Type": "application/json",
                    },
                    json={
                        "model": model,
                        "messages": [{"role": "user", "content": "Reply with OK."}],
                        "max_tokens": 16,
                    },
                    timeout=30.0,
                )
                response.raise_for_status()
                payload = response.json()
                if not payload.get("choices"):
                    raise ValueError("No Chat Completions choices in API response")
                result = "API connection successful (Chat Completions)."
                success = True
            except httpx.HTTPStatusError as exc:
                result = f"API returned HTTP {exc.response.status_code}; check key, model and endpoint."
                success = False
            except (httpx.HTTPError, ValueError, KeyError) as exc:
                # Never include API keys in logs or status messages.
                result = f"API test failed: {type(exc).__name__}. Check endpoint and model."
                success = False

            def update_ui():
                self.test_api_button.configure(state="normal")
                self.api_test_status.configure(
                    text=result, text_color="#4ade80" if success else "#f87171"
                )

            self.after(0, update_ui)

        Thread(target=worker, daemon=True).start()

    def _browse_rom(self):
        """Open file dialog to select ROM."""
        filename = filedialog.askopenfilename(
            title="Select GBA ROM",
            filetypes=[("GBA ROM files", "*.gba"), ("All files", "*.*")],
        )
        if filename:
            self.rom_entry.delete(0, "end")
            self.rom_entry.insert(0, filename)

    def _browse_output(self):
        """Open directory dialog to select output directory."""
        dirname = filedialog.askdirectory(
            title="Select Output Directory",
        )
        if dirname:
            self.output_entry.delete(0, "end")
            self.output_entry.insert(0, dirname)

    def _toggle_advanced(self):
        """Toggle advanced settings visibility."""
        if self.advanced_visible:
            self.advanced_frame.pack_forget()
            self.advanced_button.configure(text="+ Advanced")
            self.advanced_visible = False
        else:
            self.advanced_frame.pack(fill="x")
            self.advanced_button.configure(text="- Advanced")
            self.advanced_visible = True

    def _lang_name_to_code(self, name: str) -> str:
        return LANGUAGES.get(name, "en")

    def get_config(self) -> TranslationConfig:
        """Get current configuration."""
        provider = self.provider.get()
        preset = PROVIDER_PRESETS.get(provider)
        api_key = self.api_key_entry.get().strip()
        base_url = normalize_api_base(self.api_base_entry.get())

        # If user selected output dir, put work dir next to it
        output_dir = Path(self.output_entry.get()) if self.output_entry.get() else None
        work_dir = None
        if output_dir:
            work_dir = output_dir.parent / "work"

        defaults = TranslationConfig()
        return TranslationConfig(
            source_lang=self._lang_name_to_code(self.source_lang.get()),
            target_lang=self._lang_name_to_code(self.target_lang.get()),
            # Custom API uses the existing OpenAI-compatible request implementation.
            provider="openai" if provider == CUSTOM_PROVIDER else (provider or None),
            model=self.model_entry.get().strip() or (preset[1] if preset else None),
            api_base=base_url,
            api_key_env=preset[2] if preset else "OPENAI_API_KEY",
            api_key=api_key if api_key else None,
            batch_size=int(self.batch_size.get()) if self.batch_size.get().isdigit() else 30,
            max_workers=int(self.max_workers.get()) if self.max_workers.get().isdigit() else 10,
            rom_path=Path(self.rom_entry.get()) if self.rom_entry.get() else None,
            output_dir=output_dir or defaults.output_dir,
            work_dir=work_dir or defaults.work_dir,
        )

    def validate(self) -> tuple[bool, str]:
        """Validate configuration."""
        if not self.rom_entry.get():
            return False, "Please select a ROM file"
        rom_path = Path(self.rom_entry.get())
        if not rom_path.exists():
            return False, f"ROM file not found: {rom_path}"
        try:
            self._get_api_settings()
        except ValueError as exc:
            return False, str(exc)
        return True, ""
