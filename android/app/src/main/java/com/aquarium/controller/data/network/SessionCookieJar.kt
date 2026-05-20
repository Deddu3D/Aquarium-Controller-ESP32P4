package com.aquarium.controller.data.network

import okhttp3.Cookie
import okhttp3.CookieJar
import okhttp3.HttpUrl
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class SessionCookieJar @Inject constructor() : CookieJar {

    private val cookies = mutableListOf<Cookie>()

    @Synchronized
    override fun saveFromResponse(url: HttpUrl, cookies: List<Cookie>) {
        val session = cookies.firstOrNull { it.name == "session" }
        if (session != null) {
            this.cookies.clear()
            this.cookies.add(session)
        }
    }

    @Synchronized
    override fun loadForRequest(url: HttpUrl): List<Cookie> = cookies.toList()

    @Synchronized
    fun clearSession() {
        cookies.clear()
    }

    @Synchronized
    fun hasSession(): Boolean = cookies.any { it.name == "session" }
}
