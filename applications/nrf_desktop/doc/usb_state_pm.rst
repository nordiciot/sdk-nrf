.. _nrf_desktop_usb_state_pm:

USB state power manager module
##############################

.. contents::
   :local:
   :depth: 2

The |usb_state_pm| is minor, stateless module that imposes an application power level restriction related to the USB state.
The application power level is managed by the :ref:`power manager module <caf_power_manager>`.

Configuration
*************

The module is enabled by selecting :kconfig:`CONFIG_DESKTOP_USB_PM_ENABLE`.
It depends on :kconfig:`CONFIG_DESKTOP_USB_ENABLE` and :kconfig:`CONFIG_CAF_POWER_MANAGER`.

The log level is inherited from the :ref:`nrf_desktop_usb_state`.

Implementation details
**********************

For the change of the restricted power level, the module reacts to :c:struct:`usb_state_event`.
Upon reception of the event and depending on the current USB state, the module requests different power restrictions:

* If the USB state is set to :c:enum:`USB_STATE_POWERED` or :c:enum:`USB_STATE_ACTIVE`, the :c:enum:`POWER_MANAGER_LEVEL_ALIVE` is required.
* If the USB state is set to :c:enum:`USB_STATE_DISCONNECTED`, any power level is allowed.
* If the USB state is set to :c:enum:`USB_STATE_SUSPENDED`, the :c:enum:`POWER_MANAGER_LEVEL_SUSPENDED` is imposed.
  The module restricts the power down level to the :c:enum:`POWER_MANAGER_LEVEL_SUSPENDED` and generates ``force_power_down_event`` after 1 second.

For more information about the USB states in nRF Desktop, see the :ref:`nrf_desktop_usb_state`.

.. |usb_state_pm| replace:: USB state power manager module
