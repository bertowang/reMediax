package com.tencent.tgcapture.demo

import android.os.Build
import android.os.Bundle
import androidx.preference.EditTextPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.PreferenceGroup

/** 开播参数设置页（res/xml/preferences.xml）。直播期间整页置灰，避免"改了不生效"的误解。 */
class SettingsFragment : PreferenceFragmentCompat() {

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.preferences, rootKey)

        findPreference<EditTextPreference>("pref_publish_url")?.setOnBindEditTextListener {
            it.setSingleLine()
            it.setSelection(it.text.length)
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            findPreference<Preference>("pref_audio_speaker")?.apply {
                isEnabled = false
                summary = getString(R.string.pref_speaker_unsupported)
            }
        }
    }

    fun setLocked(locked: Boolean) {
        preferenceScreen?.let { setEnabledRecursive(it, !locked) }
    }

    private fun setEnabledRecursive(group: PreferenceGroup, enabled: Boolean) {
        for (i in 0 until group.preferenceCount) {
            val p = group.getPreference(i)
            if (p.key == "pref_audio_speaker" && Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) continue
            p.isEnabled = enabled
        }
    }
}
