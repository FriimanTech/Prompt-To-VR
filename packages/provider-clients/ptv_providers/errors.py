"""Shared exception types."""

from __future__ import annotations


class ProviderConfigError(RuntimeError):
    """Missing API key, bad model id, etc."""


class ProviderCallError(RuntimeError):
    """The provider returned an error or a malformed response."""
