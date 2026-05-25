package com.aquarium.controller

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.aquarium.controller.databinding.ActivitySetupBinding

class SetupActivity : AppCompatActivity() {

    private lateinit var binding: ActivitySetupBinding
    private var editMode = false

    companion object {
        const val EXTRA_EDIT_MODE = "edit_mode"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivitySetupBinding.inflate(layoutInflater)
        setContentView(binding.root)

        editMode = intent.getBooleanExtra(EXTRA_EDIT_MODE, false)

        if (editMode) {
            setSupportActionBar(binding.toolbar)
            supportActionBar?.setDisplayHomeAsUpEnabled(true)
            supportActionBar?.title = getString(R.string.change_url_title)
            // Pre-fill with the current URL
            val prefs = getSharedPreferences(MainActivity.PREFS_NAME, Context.MODE_PRIVATE)
            val current = prefs.getString(MainActivity.KEY_URL, "") ?: ""
            binding.urlInput.setText(current)
        } else {
            binding.toolbar.title = getString(R.string.setup_title)
        }

        binding.urlInput.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                binding.urlLayout.error = null
            }
            override fun afterTextChanged(s: Editable?) {}
        })

        binding.btnSave.setOnClickListener {
            val url = binding.urlInput.text?.toString()?.trim() ?: ""
            if (validateAndSave(url)) {
                if (editMode) {
                    finish()
                } else {
                    startActivity(Intent(this, MainActivity::class.java))
                    finish()
                }
            }
        }
    }

    private fun validateAndSave(url: String): Boolean {
        if (url.isBlank()) {
            binding.urlLayout.error = getString(R.string.error_url_empty)
            return false
        }
        if (!url.startsWith("http://") && !url.startsWith("https://")) {
            binding.urlLayout.error = getString(R.string.error_url_invalid)
            return false
        }
        // Normalise: ensure no trailing slash mismatch with ESP routes
        val normalised = url.trimEnd('/')

        val prefs = getSharedPreferences(MainActivity.PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit().putString(MainActivity.KEY_URL, normalised).apply()

        Toast.makeText(this, getString(R.string.url_saved), Toast.LENGTH_SHORT).show()
        return true
    }

    override fun onSupportNavigateUp(): Boolean {
        onBackPressedDispatcher.onBackPressed()
        return true
    }
}
