package com.aquarium.controller.data.model

import java.util.UUID

/**
 * Represents a configured ESP32 Aquarium Controller device.
 * Connection is always via DuckDNS HTTPS.
 */
data class EspDevice(
    val id: String = UUID.randomUUID().toString(),
    val name: String = "My Aquarium",
    /** DuckDNS subdomain only, e.g. "my-aquarium" (without .duckdns.org) */
    val duckDnsDomain: String = ""
) {
    /** Full HTTPS base URL used for all API calls, e.g. https://my-aquarium.duckdns.org/ */
    val baseUrl: String
        get() = "https://$duckDnsDomain.duckdns.org/"
}
