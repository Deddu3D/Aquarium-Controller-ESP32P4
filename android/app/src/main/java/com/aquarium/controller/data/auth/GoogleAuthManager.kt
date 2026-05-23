package com.aquarium.controller.data.auth

import android.content.Context
import android.content.Intent
import com.google.android.gms.auth.api.signin.GoogleSignIn
import com.google.android.gms.auth.api.signin.GoogleSignInAccount
import com.google.android.gms.auth.api.signin.GoogleSignInClient
import com.google.android.gms.auth.api.signin.GoogleSignInOptions
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Wraps Google Sign-In operations using the play-services-auth library.
 *
 * The sign-in flow requires launching an Activity result, so the actual
 * [signInIntent] is returned to be launched from the UI layer.
 *
 * No server-side validation is performed: the Google account is used only
 * to identify the local user and associate their ESP device list.
 */
@Singleton
class GoogleAuthManager @Inject constructor(
    @ApplicationContext private val context: Context
) {
    private val gso: GoogleSignInOptions = GoogleSignInOptions.Builder(
        GoogleSignInOptions.DEFAULT_SIGN_IN
    )
        .requestEmail()
        .requestProfile()
        .build()

    private val client: GoogleSignInClient = GoogleSignIn.getClient(context, gso)

    /** Intent to launch with [ActivityResultLauncher] to start the sign-in flow. */
    val signInIntent: Intent get() = client.signInIntent

    /** Returns the last signed-in account, or null if the user has not signed in. */
    fun getLastSignedInAccount(): GoogleSignInAccount? =
        GoogleSignIn.getLastSignedInAccount(context)

    /** Parse a [GoogleSignInAccount] from the result of the sign-in intent. */
    fun getAccountFromIntent(data: Intent?): GoogleSignInAccount? {
        return try {
            GoogleSignIn.getSignedInAccountFromIntent(data).result
        } catch (e: Exception) {
            null
        }
    }

    /** Sign out silently; call this from a coroutine. */
    fun signOut(onComplete: () -> Unit) {
        client.signOut().addOnCompleteListener { onComplete() }
    }
}
