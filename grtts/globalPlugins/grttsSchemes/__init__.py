# -*- coding: UTF-8 -*-
# globalPlugins/grttsSchemes - 广荣tts 语音方案管理
#
# 提供"语音方案管理"入口（NVDA 语音设置面板里的按钮 + 首选项菜单项），
# 弹出对话框新建/编辑/删除方案。方案数据由 grtts 合成器驱动持有（schemeStore），
# 本插件只负责界面与调用驱动的 onSchemesChanged。

import re

import addonHandler
import globalPluginHandler
import globalVars
import gui
from gui import guiHelper
from logHandler import log
import wx

addonHandler.initTranslation()

DIALOG_TITLE = _("广荣tts语音方案管理")

# 引擎内部名 → 显示名
ENGINE_DISPLAY = {
    "YongDe": _("永德"),
    "Xunfei": _("讯飞"),
    "Cerence": "Cerence",
    "vocalizer_expressive2": "Vocalizer Expressive 2",
    "NeuralVoices": "NeuralVoices",
    "SAPI5": "SAPI5",
    "DECtalk": "DECtalk",
    "RSynth": "RSynth",
}


def _engine_label(engine_name):
    return ENGINE_DISPLAY.get(engine_name, engine_name)


def _available_engines():
    try:
        from synthDrivers import _grtts as backend
        return list(backend.get_available_engines().keys())
    except Exception:
        log.debugWarning("grtts: failed to list engines", exc_info=True)
        return []


def _voices_for_engine(engine_name):
    if not engine_name:
        return []
    try:
        from synthDrivers import _grtts as backend
        voices = backend.get_available_voices_for_engine(engine_name)
    except Exception:
        log.debugWarning("grtts: failed to list voices", exc_info=True)
        return []
    names = []
    for voice_id, info in voices.items():
        if isinstance(info, (tuple, list)) and info:
            names.append(str(info[0]))
        else:
            names.append(str(voice_id))
    return names


# DLL 不提供语音语言接口，语言说明只能从语音名推断：
# 名称里的语言关键词或 zh/en 语言区域标记优先，纯汉字名视为中文，
# 仍无线索时按单语言引擎兜底，兜底也不命中则不加说明。
_KEYWORD_LANGUAGES = (
    (("cantonese", "粤语", "广东话"), _("粤语")),
    (("chinese", "mandarin", "普通话", "中文", "汉语", "国语"), _("中文")),
    (("english", "英语", "英文"), _("英语")),
)
_LOCALE_LANGUAGE_RE = re.compile(
    r"(?<![A-Za-z])(zh|yue|en)(?:[-_][A-Za-z]{2,8})?(?![A-Za-z])", re.IGNORECASE)
_ENGINE_LANGUAGE = {
    "YongDe": _("中文"),
    "DECtalk": _("英语"),
    "RSynth": _("英语"),
}


def _voice_language_label(engine_name, voice_name):
    """推断语音对应的语言文字说明，推断不出返回空串。"""
    name = str(voice_name or "").strip()
    if not name:
        return ""
    lowered = name.lower()
    for keywords, label in _KEYWORD_LANGUAGES:
        if any(keyword in lowered for keyword in keywords):
            return label
    match = _LOCALE_LANGUAGE_RE.search(name)
    if match:
        prefix = match.group(1).lower()
        if prefix == "zh":
            return _("中文")
        if prefix == "yue":
            return _("粤语")
        return _("英语")
    if any("\u4e00" <= char <= "\u9fff" for char in name):
        return _("中文")
    return _ENGINE_LANGUAGE.get(engine_name, "")


def _engine_defaults(engine_name, english=False):
    """该引擎的默认参数（如 Cerence 默认音调 33、音量 70）。无则返回 None。"""
    if not engine_name:
        return None
    try:
        from synthDrivers import _grtts as backend
        return backend.get_engine_defaults(engine_name, english=english)
    except Exception:
        log.debugWarning("grtts: failed to get engine defaults", exc_info=True)
        return None


def _get_grtts_synth():
    try:
        import synthDriverHandler
        synth = synthDriverHandler.getSynth()
    except Exception:
        return None
    if synth is None or getattr(synth, "name", None) != "grtts":
        return None
    return synth


def openManager(parent):
    """打开方案管理对话框；当前合成器不是广荣tts 时给出提示。"""
    synth = _get_grtts_synth()
    if synth is None:
        gui.messageBox(
            _("请先在“语音合成器”里切换到广荣tts，再管理语音方案。"),
            DIALOG_TITLE, wx.OK | wx.ICON_INFORMATION,
        )
        return
    if getattr(synth, "schemeStore", None) is None:
        gui.messageBox(
            _("当前合成器不支持语音方案管理。"),
            DIALOG_TITLE, wx.OK | wx.ICON_INFORMATION,
        )
        return
    dialog = SchemeManagerDialog(parent if parent else gui.mainFrame, synth)
    try:
        dialog.ShowModal()
    finally:
        dialog.Destroy()


class SchemeEditDialog(wx.Dialog):
    """新建/编辑单个方案。"""

    def __init__(self, parent, store, synth, scheme=None):
        from synthDrivers._grtts_schemes import Scheme
        self._store = store
        self._synth = synth
        self._creating = scheme is None
        self._scheme = Scheme() if self._creating else scheme.copy()
        self.resultScheme = None
        title = _("新建方案") if self._creating else _("编辑方案")
        super().__init__(parent, title=title, style=wx.DEFAULT_DIALOG_STYLE | wx.RESIZE_BORDER)

        self._engines = _available_engines()
        engine_labels = [_engine_label(e) for e in self._engines]

        mainSizer = wx.BoxSizer(wx.VERTICAL)
        sHelper = guiHelper.BoxSizerHelper(self, orientation=wx.VERTICAL)

        self._nameCtrl = sHelper.addLabeledControl(
            _("方案名称(&N):"), wx.TextCtrl, value=self._scheme.name)

        self._cnEngineChoice = sHelper.addLabeledControl(
            _("中文引擎(&E):"), wx.Choice, choices=engine_labels)
        self._cnVoiceChoice = sHelper.addLabeledControl(
            _("中文语音(&V):"), wx.Choice, choices=[])
        self._cnEngineChoice.Bind(wx.EVT_CHOICE, self._on_cn_engine_changed)

        self._dualCheck = wx.CheckBox(self, label=_("启用英文双语音(&D)"))
        self._dualCheck.SetValue(self._scheme.dualVoice)
        self._dualCheck.Bind(wx.EVT_CHECKBOX, lambda evt: self._update_dual_enabled())
        sHelper.addItem(self._dualCheck)

        self._enEngineChoice = sHelper.addLabeledControl(
            _("英文引擎(&G):"), wx.Choice, choices=engine_labels)
        self._enVoiceChoice = sHelper.addLabeledControl(
            _("英文语音(&I):"), wx.Choice, choices=[])
        self._enEngineChoice.Bind(wx.EVT_CHOICE, self._on_en_engine_changed)

        # 中文参数
        self._rateSlider = sHelper.addLabeledControl(
            _("语速(&R):"), wx.Slider, minValue=0, maxValue=100)
        self._rateSlider.SetValue(int(self._scheme.get("rate")))
        self._pitchSlider = sHelper.addLabeledControl(
            _("音调(&P):"), wx.Slider, minValue=0, maxValue=100)
        self._pitchSlider.SetValue(int(self._scheme.get("pitch")))
        self._volumeSlider = sHelper.addLabeledControl(
            _("音量(&L):"), wx.Slider, minValue=0, maxValue=100)
        self._volumeSlider.SetValue(int(self._scheme.get("volume")))
        self._rateMulCtrl = sHelper.addLabeledControl(
            _("语速倍率(&M):"), wx.SpinCtrl, min=1, max=10,
            initial=int(self._scheme.get("rateMultiplier")))

        # 英文参数
        self._enRateSlider = sHelper.addLabeledControl(
            _("英文语速(&1):"), wx.Slider, minValue=0, maxValue=100)
        self._enRateSlider.SetValue(int(self._scheme.get("englishRate")))
        self._enPitchSlider = sHelper.addLabeledControl(
            _("英文音调(&2):"), wx.Slider, minValue=0, maxValue=100)
        self._enPitchSlider.SetValue(int(self._scheme.get("englishPitch")))
        self._enVolumeSlider = sHelper.addLabeledControl(
            _("英文音量(&3):"), wx.Slider, minValue=0, maxValue=100)
        self._enVolumeSlider.SetValue(int(self._scheme.get("englishVolume")))
        self._enRateMulCtrl = sHelper.addLabeledControl(
            _("英文语速倍率(&4):"), wx.SpinCtrl, min=1, max=10,
            initial=int(self._scheme.get("englishRateMultiplier")))

        # 试听按钮：用当前配置临时合成一段中英示例
        self._previewButton = wx.Button(self, label=_("试听(&T)"))
        self._previewButton.Bind(wx.EVT_BUTTON, lambda evt: self._on_preview())
        sHelper.addItem(self._previewButton)

        # 初始选择引擎与语音
        self._select_engine(self._cnEngineChoice, self._scheme.cnEngine)
        self._populate_voices(self._cnVoiceChoice, self._selected_engine(self._cnEngineChoice), self._scheme.cnVoice)
        self._select_engine(self._enEngineChoice, self._scheme.enEngine or self._scheme.cnEngine)
        self._populate_voices(self._enVoiceChoice, self._selected_engine(self._enEngineChoice), self._scheme.enVoice)
        self._update_dual_enabled()

        # 新建方案时，用所选引擎的默认参数初始化各滑块（如 Cerence 默认音调 33）
        if self._creating:
            self._apply_main_engine_defaults(self._selected_engine(self._cnEngineChoice))
            self._apply_english_engine_defaults(self._selected_engine(self._enEngineChoice))

        mainSizer.Add(sHelper.sizer, border=guiHelper.BORDER_FOR_DIALOGS, flag=wx.ALL | wx.EXPAND, proportion=1)

        buttonSizer = self.CreateButtonSizer(wx.OK | wx.CANCEL)
        mainSizer.Add(buttonSizer, border=guiHelper.BORDER_FOR_DIALOGS, flag=wx.ALL | wx.ALIGN_RIGHT)
        self.Bind(wx.EVT_BUTTON, self._on_ok, id=wx.ID_OK)

        self.SetSizerAndFit(mainSizer)
        self.CentreOnParent()
        self._nameCtrl.SetFocus()

    # ---- 引擎/语音联动 ----
    def _selected_engine(self, choice):
        index = choice.GetSelection()
        if 0 <= index < len(self._engines):
            return self._engines[index]
        return ""

    def _select_engine(self, choice, engine_name):
        if engine_name in self._engines:
            choice.SetSelection(self._engines.index(engine_name))
        elif self._engines:
            choice.SetSelection(0)

    def _populate_voices(self, voice_choice, engine_name, preferred):
        names = _voices_for_engine(engine_name)
        # 弹出菜单里语音名后追加语言说明；原始语音名另存，按选中下标映射回去
        labels = []
        for name in names:
            lang = _voice_language_label(engine_name, name)
            labels.append("{0}（{1}）".format(name, lang) if lang else name)
        voice_choice._grttsVoiceNames = names
        voice_choice.Set(labels)
        if names:
            index = names.index(preferred) if preferred in names else 0
            voice_choice.SetSelection(index)

    def _selected_voice_name(self, voice_choice):
        names = getattr(voice_choice, "_grttsVoiceNames", None)
        index = voice_choice.GetSelection()
        if names and 0 <= index < len(names):
            return names[index]
        return voice_choice.GetStringSelection()

    def _on_cn_engine_changed(self, evt):
        engine = self._selected_engine(self._cnEngineChoice)
        self._populate_voices(self._cnVoiceChoice, engine, None)
        self._apply_main_engine_defaults(engine)

    def _on_en_engine_changed(self, evt):
        engine = self._selected_engine(self._enEngineChoice)
        self._populate_voices(self._enVoiceChoice, engine, None)
        self._apply_english_engine_defaults(engine)

    def _apply_main_engine_defaults(self, engine_name):
        defaults = _engine_defaults(engine_name, english=False)
        if not defaults:
            return
        if "rate" in defaults:
            self._rateSlider.SetValue(int(defaults["rate"]))
        if "pitch" in defaults:
            self._pitchSlider.SetValue(int(defaults["pitch"]))
        if "volume" in defaults:
            self._volumeSlider.SetValue(int(defaults["volume"]))
        if "rateMultiplier" in defaults:
            self._rateMulCtrl.SetValue(int(defaults["rateMultiplier"]))

    def _apply_english_engine_defaults(self, engine_name):
        defaults = _engine_defaults(engine_name, english=True)
        if not defaults:
            return
        if "rate" in defaults:
            self._enRateSlider.SetValue(int(defaults["rate"]))
        if "pitch" in defaults:
            self._enPitchSlider.SetValue(int(defaults["pitch"]))
        if "volume" in defaults:
            self._enVolumeSlider.SetValue(int(defaults["volume"]))
        if "rateMultiplier" in defaults:
            self._enRateMulCtrl.SetValue(int(defaults["rateMultiplier"]))

    def _update_dual_enabled(self):
        enabled = self._dualCheck.GetValue()
        for control in (
            self._enEngineChoice, self._enVoiceChoice,
            self._enRateSlider, self._enPitchSlider,
            self._enVolumeSlider, self._enRateMulCtrl,
        ):
            control.Enable(enabled)

    # ---- 试听 / 确定 ----
    def _build_scheme_from_controls(self):
        """把当前各控件的值写入工作副本并返回（不做校验）。"""
        scheme = self._scheme
        scheme.name = self._nameCtrl.GetValue().strip()
        scheme.cnEngine = self._selected_engine(self._cnEngineChoice)
        scheme.cnVoice = self._selected_voice_name(self._cnVoiceChoice)
        scheme.dualVoice = bool(self._dualCheck.GetValue())
        scheme.enEngine = self._selected_engine(self._enEngineChoice)
        scheme.enVoice = self._selected_voice_name(self._enVoiceChoice)
        scheme.set("rate", self._rateSlider.GetValue())
        scheme.set("pitch", self._pitchSlider.GetValue())
        scheme.set("volume", self._volumeSlider.GetValue())
        scheme.set("rateMultiplier", self._rateMulCtrl.GetValue())
        scheme.set("englishRate", self._enRateSlider.GetValue())
        scheme.set("englishPitch", self._enPitchSlider.GetValue())
        scheme.set("englishVolume", self._enVolumeSlider.GetValue())
        scheme.set("englishRateMultiplier", self._enRateMulCtrl.GetValue())
        return scheme

    def _on_preview(self):
        if self._synth is None:
            return
        try:
            self._synth.previewScheme(self._build_scheme_from_controls())
        except Exception:
            log.debugWarning("grtts: preview failed", exc_info=True)

    def _on_ok(self, evt):
        name = self._nameCtrl.GetValue().strip()
        if not name:
            gui.messageBox(_("请填写方案名称。"), DIALOG_TITLE, wx.OK | wx.ICON_ERROR)
            self._nameCtrl.SetFocus()
            return
        if self._store.name_exists(name, exclude_id=self._scheme.id):
            gui.messageBox(_("已存在同名方案，请换一个名称。"), DIALOG_TITLE, wx.OK | wx.ICON_ERROR)
            self._nameCtrl.SetFocus()
            return
        self.resultScheme = self._build_scheme_from_controls()
        self.EndModal(wx.ID_OK)


class SchemeManagerDialog(wx.Dialog):
    """方案列表 + 新建/编辑/删除。"""

    def __init__(self, parent, synth):
        super().__init__(parent, title=DIALOG_TITLE, style=wx.DEFAULT_DIALOG_STYLE | wx.RESIZE_BORDER)
        self._synth = synth
        self._store = synth.schemeStore
        self._schemes = []

        mainSizer = wx.BoxSizer(wx.VERTICAL)
        contentSizer = wx.BoxSizer(wx.HORIZONTAL)

        listHelper = guiHelper.BoxSizerHelper(self, orientation=wx.VERTICAL)
        self._list = listHelper.addLabeledControl(
            _("语音方案(&S):"), wx.ListBox, style=wx.LB_SINGLE, size=(260, 240))
        self._list.Bind(wx.EVT_LISTBOX, lambda evt: self._update_buttons())
        self._list.Bind(wx.EVT_LISTBOX_DCLICK, lambda evt: self._on_edit())
        contentSizer.Add(listHelper.sizer, flag=wx.EXPAND | wx.ALL, border=4, proportion=1)

        btnSizer = wx.BoxSizer(wx.VERTICAL)
        self._newBtn = wx.Button(self, label=_("新建(&N)..."))
        self._editBtn = wx.Button(self, label=_("编辑(&E)..."))
        self._deleteBtn = wx.Button(self, label=_("删除(&D)"))
        self._newBtn.Bind(wx.EVT_BUTTON, lambda evt: self._on_new())
        self._editBtn.Bind(wx.EVT_BUTTON, lambda evt: self._on_edit())
        self._deleteBtn.Bind(wx.EVT_BUTTON, lambda evt: self._on_delete())
        for btn in (self._newBtn, self._editBtn, self._deleteBtn):
            btnSizer.Add(btn, flag=wx.EXPAND | wx.BOTTOM, border=6)
        contentSizer.Add(btnSizer, flag=wx.ALL, border=4)

        mainSizer.Add(contentSizer, flag=wx.EXPAND | wx.ALL, border=guiHelper.BORDER_FOR_DIALOGS, proportion=1)

        closeSizer = self.CreateButtonSizer(wx.CLOSE)
        mainSizer.Add(closeSizer, flag=wx.ALL | wx.ALIGN_RIGHT, border=guiHelper.BORDER_FOR_DIALOGS)
        self.Bind(wx.EVT_BUTTON, lambda evt: self.EndModal(wx.ID_CLOSE), id=wx.ID_CLOSE)
        self.SetEscapeId(wx.ID_CLOSE)

        self.SetSizerAndFit(mainSizer)
        self.CentreOnParent()

        self._refresh_list(select_id=getattr(self._synth, "_currentSchemeId", None))
        self._list.SetFocus()

    def _display_name(self, scheme):
        if scheme.isDefault:
            return _("{name}（默认）").format(name=scheme.name)
        return scheme.name

    def _refresh_list(self, select_id=None):
        self._schemes = self._store.list()
        self._list.Set([self._display_name(s) for s in self._schemes])
        if self._schemes:
            index = 0
            for i, scheme in enumerate(self._schemes):
                if scheme.id == select_id:
                    index = i
                    break
            self._list.SetSelection(index)
        self._update_buttons()

    def _selected_scheme(self):
        index = self._list.GetSelection()
        if 0 <= index < len(self._schemes):
            return self._schemes[index]
        return None

    def _update_buttons(self):
        scheme = self._selected_scheme()
        self._editBtn.Enable(scheme is not None)
        self._deleteBtn.Enable(scheme is not None and self._store.can_delete(scheme.id))

    def _on_new(self):
        dialog = SchemeEditDialog(self, self._store, self._synth, None)
        try:
            if dialog.ShowModal() == wx.ID_OK and dialog.resultScheme is not None:
                added = self._store.add(dialog.resultScheme)
                self._synth.onSchemesChanged(None)
                self._refresh_list(select_id=added.id)
            else:
                self._synth.endPreview()
        finally:
            dialog.Destroy()

    def _on_edit(self):
        scheme = self._selected_scheme()
        if scheme is None:
            return
        dialog = SchemeEditDialog(self, self._store, self._synth, scheme)
        try:
            if dialog.ShowModal() == wx.ID_OK and dialog.resultScheme is not None:
                self._store.replace(dialog.resultScheme)
                self._synth.onSchemesChanged(None)
                self._refresh_list(select_id=dialog.resultScheme.id)
            else:
                self._synth.endPreview()
        finally:
            dialog.Destroy()

    def _on_delete(self):
        scheme = self._selected_scheme()
        if scheme is None:
            return
        if not self._store.can_delete(scheme.id):
            gui.messageBox(
                _("默认方案不可删除，且至少要保留一个方案。"),
                DIALOG_TITLE, wx.OK | wx.ICON_INFORMATION,
            )
            return
        confirm = gui.messageBox(
            _("确定删除方案“{name}”吗？").format(name=scheme.name),
            DIALOG_TITLE, wx.YES_NO | wx.ICON_QUESTION,
        )
        if confirm != wx.YES:
            return
        self._store.delete(scheme.id)
        self._synth.onSchemesChanged(None)
        self._refresh_list(select_id=getattr(self._synth, "_currentSchemeId", None))


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

    def __init__(self):
        super().__init__()
        self._menuItem = None
        self._patchedPanelCls = None
        if globalVars.appArgs.secure:
            return
        self._install_menu_item()
        self._patch_voice_panel()

    def terminate(self):
        try:
            if self._menuItem is not None:
                gui.mainFrame.sysTrayIcon.preferencesMenu.Remove(self._menuItem)
        except Exception:
            log.debugWarning("grtts: failed to remove scheme manager menu item", exc_info=True)
        self._menuItem = None
        self._unpatch_voice_panel()
        super().terminate()

    def _install_menu_item(self):
        try:
            prefsMenu = gui.mainFrame.sysTrayIcon.preferencesMenu
            self._menuItem = prefsMenu.Append(wx.ID_ANY, _("广荣tts语音方案管理(&S)..."))
            gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self._on_menu, self._menuItem)
        except Exception:
            log.error("grtts: failed to add scheme manager menu item", exc_info=True)

    def _on_menu(self, evt):
        gui.mainFrame.prePopup()
        try:
            openManager(gui.mainFrame)
        finally:
            gui.mainFrame.postPopup()

    def _patch_voice_panel(self):
        """向 NVDA 语音设置面板末尾注入"语音方案管理"按钮（仅当前合成器为 grtts 时显示）。"""
        try:
            from gui import settingsDialogs
        except Exception:
            return
        panel_cls = getattr(settingsDialogs, "VoiceSettingsPanel", None)
        if panel_cls is None:
            return
        original = panel_cls.makeSettings
        if getattr(original, "_grttsPatched", False):
            self._patchedPanelCls = panel_cls
            return

        def makeSettings(panel, settingsSizer, __original=original):
            __original(panel, settingsSizer)
            try:
                if _get_grtts_synth() is None:
                    return
                helper = guiHelper.BoxSizerHelper(panel, sizer=settingsSizer)
                button = wx.Button(panel, label=_("语音方案管理(&M)..."))
                button.Bind(
                    wx.EVT_BUTTON,
                    lambda evt: openManager(panel.GetTopLevelParent()),
                )
                helper.addItem(button)
            except Exception:
                log.debugWarning("grtts: failed to inject scheme manager button", exc_info=True)

        makeSettings._grttsPatched = True
        makeSettings._grttsOriginal = original
        panel_cls.makeSettings = makeSettings
        self._patchedPanelCls = panel_cls

    def _unpatch_voice_panel(self):
        panel_cls = self._patchedPanelCls
        self._patchedPanelCls = None
        if panel_cls is None:
            return
        try:
            current = getattr(panel_cls, "makeSettings", None)
            original = getattr(current, "_grttsOriginal", None)
            if original is not None:
                panel_cls.makeSettings = original
        except Exception:
            log.debugWarning("grtts: failed to restore voice panel", exc_info=True)
