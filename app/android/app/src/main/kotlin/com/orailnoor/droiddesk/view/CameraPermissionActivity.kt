package com.orailnoor.droiddesk.view

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.os.Bundle
import com.orailnoor.droiddesk.runtime.CameraBridge

/**
 * Asks for the CAMERA permission when a Linux program first opens the camera (CameraBridge
 * runs in a service, which cannot show the dialog). Has no UI of its own: only Android's
 * permission dialog appears, and the activity ends with the answer.
 */
class CameraPermissionActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        CameraBridge.permissionDialogShown()
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            CameraBridge.permissionDialogAnswered()
            finish()
            return
        }
        if (savedInstanceState == null) requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        CameraBridge.permissionDialogAnswered()
        finish()
    }

    override fun onDestroy() {
        if (isFinishing) CameraBridge.permissionDialogAnswered()
        super.onDestroy()
    }

    private companion object {
        const val REQUEST_CAMERA = 7304
    }
}
