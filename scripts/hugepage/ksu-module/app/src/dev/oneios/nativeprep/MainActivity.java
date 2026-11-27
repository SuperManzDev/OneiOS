package dev.oneios.nativeprep;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.Icon;
import android.graphics.drawable.RippleDrawable;
import android.os.Bundle;
import android.text.method.ScrollingMovementMethod;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.Collections;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public final class MainActivity extends Activity {
    private static final String ACTION_PREPARE = "dev.oneios.nativeprep.PREPARE";
    private static final String ACTION_LAUNCH = "dev.oneios.nativeprep.LAUNCH";
    private static final String ACTION_PIN = "dev.oneios.nativeprep.PIN";
    private static final String MODULE_COMMAND = "/data/adb/modules/oneios_native_on_demand/system/bin/oneios-native";
    private static final String COMMAND_PICKER =
            "if [ -x /system/bin/oneios-native ]; then C=/system/bin/oneios-native; " +
            "elif [ -r " + MODULE_COMMAND + " ]; then C='/system/bin/sh " + MODULE_COMMAND + "'; " +
            "else echo 'OneiOS KernelSU module is not installed yet.'; exit 127; fi; exec timeout -s TERM -k 5 140 $C ";

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private TextView statusText;
    private TextView stateLabel;
    private Button prepareButton;
    private Button launchButton;
    private Button releaseButton;
    private Button refreshButton;
    private Button shortcutButton;
    private int primary;
    private int onPrimary;
    private int surface;
    private int surfaceVariant;
    private int text;
    private int secondary;
    private int outline;
    private int success;
    private int warning;
    private int error;
    private boolean readyForLaunch;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        primary = getColor(R.color.app_primary);
        onPrimary = getColor(R.color.app_on_primary);
        surface = getColor(R.color.app_surface);
        surfaceVariant = getColor(R.color.app_surface_variant);
        text = getColor(R.color.app_text);
        secondary = getColor(R.color.app_text_secondary);
        outline = getColor(R.color.app_outline);
        success = getColor(R.color.app_success);
        warning = getColor(R.color.app_warning);
        error = getColor(R.color.app_error);
        getWindow().setStatusBarColor(getColor(R.color.app_background));
        getWindow().setNavigationBarColor(getColor(R.color.app_background));
        getWindow().setDecorFitsSystemWindows(false);
        setContentView(buildContent());
        runCommand("status", "Checking host support…");
        if (ACTION_PREPARE.equals(getIntent().getAction())) {
            statusText.postDelayed(this::confirmPrepare, 350);
        } else if (ACTION_LAUNCH.equals(getIntent().getAction())) {
            statusText.postDelayed(this::confirmLaunch, 350);
        } else if (ACTION_PIN.equals(getIntent().getAction())) {
            statusText.postDelayed(this::requestShortcut, 350);
        }
    }

    private View buildContent() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(getColor(R.color.app_background));
        LinearLayout root = column();
        root.setPadding(dp(24), dp(24), dp(24), dp(36));
        root.setOnApplyWindowInsetsListener((view, windowInsets) -> {
            Insets bars = windowInsets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            view.setPadding(dp(24) + bars.left, dp(24) + bars.top, dp(24) + bars.right, dp(36) + bars.bottom);
            return WindowInsets.CONSUMED;
        });
        scroll.addView(root, matchWrap());

        LinearLayout mark = new LinearLayout(this);
        mark.setOrientation(LinearLayout.HORIZONTAL);
        mark.setGravity(Gravity.CENTER_VERTICAL);
        TextView icon = text("M", 18, Typeface.BOLD, onPrimary);
        icon.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        icon.setGravity(Gravity.CENTER);
        icon.setBackground(round(primary, 14));
        mark.addView(icon, box(48, 48));
        LinearLayout titles = column();
        titles.setPadding(dp(14), 0, 0, 0);
        TextView title = text("Native VM memory", 27, Typeface.BOLD, text);
        TextView subtitle = text("Manual host preparation for OneiOS", 15, Typeface.NORMAL, secondary);
        titles.addView(title);
        titles.addView(subtitle, topMargin(wrap(), 3));
        mark.addView(titles, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        root.addView(mark);

        TextView promise = text("Nothing runs at boot", 13, Typeface.BOLD, primary);
        promise.setGravity(Gravity.CENTER);
        promise.setPadding(dp(14), dp(8), dp(14), dp(8));
        promise.setBackground(round(surfaceVariant, 18));
        root.addView(promise, topMargin(wrap(), 24));

        LinearLayout statusCard = column();
        statusCard.setPadding(dp(18), dp(18), dp(18), dp(16));
        statusCard.setBackground(round(surface, 16));
        TextView statusTitle = text("Host support", 18, Typeface.BOLD, text);
        stateLabel = text("Checking…", 14, Typeface.BOLD, warning);
        stateLabel.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        statusText = text("Waiting for root status.", 13, Typeface.NORMAL, secondary);
        statusText.setTypeface(Typeface.MONOSPACE);
        statusText.setTextIsSelectable(true);
        statusText.setMovementMethod(new ScrollingMovementMethod());
        statusText.setMinHeight(dp(116));
        statusText.setLineSpacing(0, 1.12f);
        statusCard.addView(statusTitle);
        statusCard.addView(stateLabel, topMargin(wrap(), 8));
        statusCard.addView(statusText, topMargin(matchWrap(), 12));
        root.addView(statusCard, topMargin(matchWrap(), 18));

        TextView actionTitle = text("Memory preparation", 20, Typeface.BOLD, text);
        root.addView(actionTitle, topMargin(wrap(), 28));
        TextView actionCopy = text(
                "Prepare reserves 2 MiB pages for a 4 GiB VM. It does not stop One UI, turn off the display, or start the VM. If Android has already fragmented RAM, it fails safely.",
                15, Typeface.NORMAL, secondary);
        actionCopy.setLineSpacing(0, 1.16f);
        root.addView(actionCopy, topMargin(matchWrap(), 8));

        prepareButton = button("Prepare 4 GiB", false);
        prepareButton.setOnClickListener(v -> confirmPrepare());
        root.addView(prepareButton, topMargin(match(), 18));

        launchButton = button("Launch OneiOS", true);
        launchButton.setEnabled(false);
        launchButton.setAlpha(0.55f);
        launchButton.setOnClickListener(v -> confirmLaunch());
        root.addView(launchButton, topMargin(match(), 10));

        releaseButton = button("Release reserved memory", false);
        releaseButton.setOnClickListener(v -> confirmRelease());
        root.addView(releaseButton, topMargin(match(), 10));

        refreshButton = button("Refresh status", false);
        refreshButton.setOnClickListener(v -> runCommand("status", "Refreshing…"));
        root.addView(refreshButton, topMargin(match(), 10));

        LinearLayout safety = column();
        safety.setPadding(dp(18), dp(16), dp(18), dp(16));
        safety.setBackground(outlined(surface, outline, 14));
        TextView safetyTitle = text("Screen-safe by design", 16, Typeface.BOLD, text);
        TextView safetyCopy = text(
                "No startup service or automatic VM launch is installed. Prepare keeps One UI running. Launch is separate, warned, and protected by the phone recovery script.",
                14, Typeface.NORMAL, secondary);
        safetyCopy.setLineSpacing(0, 1.14f);
        safety.addView(safetyTitle);
        safety.addView(safetyCopy, topMargin(matchWrap(), 7));
        root.addView(safety, topMargin(matchWrap(), 24));

        shortcutButton = button("Add Launch shortcut to Home", false);
        shortcutButton.setOnClickListener(v -> requestShortcut());
        root.addView(shortcutButton, topMargin(match(), 16));

        TextView footer = text("Galaxy S25 Ultra · on-demand module", 12, Typeface.NORMAL, secondary);
        footer.setGravity(Gravity.CENTER);
        root.addView(footer, topMargin(match(), 28));
        return scroll;
    }

    private void confirmPrepare() {
        showConfirmation(
                "Prepare memory for OneiOS?",
                "This loads the reviewed support modules and requests 2,032 huge pages. The screen and One UI stay running. No VM starts. Continue?",
                "Prepare",
                () -> runCommand("prepare 4096", "Preparing 4 GiB pool…"));
    }

    private void confirmLaunch() {
        showConfirmation(
                "Launch OneiOS now?",
                "The VM image and disks are stored on this phone, so a computer is not required after staging. One UI will stop and the screen may go black until iOS posts its first frame. Hold Volume Down for 10 seconds when the key monitor is available, or wait for the 15-minute VM stop and 20-minute framework failsafe. If Android cannot recover, the last-resort failsafe may reboot the phone. Continue?",
                "Launch OneiOS",
                () -> runCommand("launch", "Handing the display to OneiOS…"));
    }

    private void confirmRelease() {
        showConfirmation(
                "Release reserved memory?",
                "Release is refused if a VM or shared parcel is active. It never force-removes a module.",
                "Release",
                () -> runCommand("release", "Releasing…"));
    }

    private void showConfirmation(String title, String message, String action, Runnable accepted) {
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(message)
                .setNegativeButton("Cancel", null)
                .setPositiveButton(action, (ignored, which) -> accepted.run())
                .create();
        dialog.setOnShowListener(ignored -> {
            Window window = dialog.getWindow();
            if (window != null) {
                window.setBackgroundDrawable(round(surface, 28));
                View decor = window.getDecorView();
                decor.setPadding(dp(4), dp(4), dp(4), dp(4));
            }
            dialog.getButton(DialogInterface.BUTTON_POSITIVE).setTextColor(primary);
            dialog.getButton(DialogInterface.BUTTON_NEGATIVE).setTextColor(primary);
        });
        dialog.show();
    }

    private void runCommand(String args, String pending) {
        setBusy(true);
        stateLabel.setText(pending);
        stateLabel.setTextColor(warning);
        statusText.setText("KernelSU may ask you to grant this app root access.\n\n" + pending);
        worker.execute(() -> {
            StringBuilder output = new StringBuilder();
            int code = -1;
            boolean timedOut = false;
            try {
                Process process = new ProcessBuilder("su", "-c", COMMAND_PICKER + args)
                        .redirectErrorStream(true)
                        .start();
                if (!process.waitFor(150, TimeUnit.SECONDS)) {
                    timedOut = true;
                    process.destroy();
                    if (!process.waitFor(2, TimeUnit.SECONDS)) process.destroyForcibly();
                } else {
                    code = process.exitValue();
                }
                try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()))) {
                    String line;
                    while ((line = reader.readLine()) != null) output.append(line).append('\n');
                }
            } catch (Exception e) {
                output.append("Unable to start KernelSU root command.\n");
                output.append(e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage()).append('\n');
            }
            boolean commandTimedOut = timedOut || code == 124;
            if (commandTimedOut) {
                output.append("\nCurrent status after timeout:\n");
                try {
                    Process status = new ProcessBuilder("su", "-c", COMMAND_PICKER + "status")
                            .redirectErrorStream(true)
                            .start();
                    if (!status.waitFor(12, TimeUnit.SECONDS)) status.destroyForcibly();
                    try (BufferedReader reader = new BufferedReader(new InputStreamReader(status.getInputStream()))) {
                        String line;
                        while ((line = reader.readLine()) != null) output.append(line).append('\n');
                    }
                } catch (Exception e) {
                    output.append("Status refresh failed; reopen the app before another action.\n");
                }
            }
            final int result = code;
            final boolean didTimeOut = commandTimedOut;
            final String message = output.length() == 0 ? "No output returned." : output.toString().trim();
            runOnUiThread(() -> applyResult(args, result, didTimeOut, message));
        });
    }

    private void applyResult(String args, int result, boolean timedOut, String message) {
        readyForLaunch = false;
        String shown = message;
        String state;
        int color;
        if (timedOut) {
            state = "Timed out — check status";
            color = error;
            shown += "\n\nThe command exceeded 150 seconds and was stopped. No VM was launched. Tap Refresh status before trying anything else.";
        } else if (result == 0 && args.equals("status")) {
            boolean none = message.contains("hugepool: not loaded") && message.contains("hostshare: not loaded") &&
                    message.contains("udmabuf: not loaded") && message.contains("halt-poll: not loaded");
            boolean ready = message.contains("hugepool: loaded") && message.contains("hostshare: loaded") &&
                    message.contains("udmabuf: loaded") && message.contains("halt-poll: loaded") &&
                    message.contains("ownership: ready");
            boolean partial = message.contains("ownership: partial") || message.contains("ownership: loading");
            if (ready) {
                readyForLaunch = message.contains("launcher: staged on phone");
                state = readyForLaunch ? "Prepared — ready to launch" : "Prepared — launcher not staged";
                color = readyForLaunch ? success : warning;
            } else if (partial || !none) {
                readyForLaunch = false;
                state = "Partially prepared — release required";
                color = error;
                shown += "\n\nTap Release reserved memory before another Prepare attempt.";
            } else {
                readyForLaunch = false;
                state = "Not prepared";
                color = warning;
            }
        } else if (result == 0 && args.startsWith("prepare") && message.contains("ready for a")) {
            readyForLaunch = message.contains("launcher: staged on phone");
            state = readyForLaunch ? "Ready for VM" : "Memory ready — stage launcher from computer";
            color = readyForLaunch ? success : warning;
        } else if (result == 0 && args.equals("launch")) {
            state = "Launching OneiOS";
            color = success;
        } else if (result == 0 && args.equals("release")) {
            readyForLaunch = false;
            state = "Released";
            color = success;
        } else if (message.contains("Cannot run program \"su\"") || message.contains("Unable to start KernelSU") ||
                message.toLowerCase().contains("permission denied") || message.toLowerCase().contains("access denied") ||
                message.toLowerCase().contains("not allowed") || message.toLowerCase().contains("root denied")) {
            state = "Root access required";
            color = error;
            shown = "Open KernelSU Manager and grant root to OneiOS Memory, then return and tap Refresh status.\n\n" + message;
        } else if (result == 127 || message.contains("not installed yet")) {
            state = "Module unavailable";
            color = error;
            shown += "\n\nInstall and activate the OneiOS on-demand KernelSU module, then refresh.";
        } else if (message.contains("fingerprint changed") || message.contains("package is for") || message.contains("module hash differs")) {
            state = "Unsupported build";
            color = error;
        } else if (message.contains("PARTIAL") || message.contains("ownership: partial") || message.contains("state is partial")) {
            state = "Partially prepared — release required";
            color = error;
        } else if (message.contains("pages are available")) {
            state = "Not enough contiguous memory";
            color = warning;
            shown += "\n\nThe screen stayed on and no VM started. Rebooting and preparing earlier may provide more contiguous memory.";
        } else {
            state = "Not prepared";
            color = error;
        }
        stateLabel.setText(state);
        stateLabel.setTextColor(color);
        statusText.setText(shown);
        setBusy(false);
    }

    private void requestShortcut() {
        ShortcutManager manager = getSystemService(ShortcutManager.class);
        if (manager == null || !manager.isRequestPinShortcutSupported()) {
            stateLabel.setText("Launcher does not support pinning");
            stateLabel.setTextColor(error);
            return;
        }
        Intent intent = new Intent(this, MainActivity.class)
                .setAction(ACTION_LAUNCH)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        ShortcutInfo shortcut = new ShortcutInfo.Builder(this, "prepare_vm_memory")
                .setShortLabel(getString(R.string.shortcut_prepare))
                .setLongLabel("Launch OneiOS from this phone")
                .setIcon(Icon.createWithResource(this, R.drawable.ic_shortcut_prepare))
                .setIntent(intent)
                .build();
        manager.updateShortcuts(Collections.singletonList(shortcut));
        Intent callbackIntent = new Intent(this, MainActivity.class).setAction("dev.oneios.nativeprep.PINNED");
        PendingIntent callback = PendingIntent.getActivity(this, 10, callbackIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        if (manager.requestPinShortcut(shortcut, callback.getIntentSender())) {
            stateLabel.setText("Confirm the shortcut in One UI");
            stateLabel.setTextColor(primary);
        }
    }

    private void setBusy(boolean busy) {
        prepareButton.setEnabled(!busy);
        launchButton.setEnabled(!busy && readyForLaunch);
        releaseButton.setEnabled(!busy);
        refreshButton.setEnabled(!busy);
        shortcutButton.setEnabled(!busy);
        float alpha = busy ? 0.55f : 1f;
        prepareButton.setAlpha(alpha);
        launchButton.setAlpha(!busy && readyForLaunch ? 1f : 0.55f);
        releaseButton.setAlpha(alpha);
        refreshButton.setAlpha(alpha);
        shortcutButton.setAlpha(alpha);
    }

    private LinearLayout column() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    private TextView text(String value, int sp, int style, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sp);
        view.setTextColor(color);
        view.setTypeface(Typeface.create("sans", style));
        return view;
    }

    private Button button(String label, boolean filled) {
        Button button = new Button(this);
        button.setText(label);
        button.setTextSize(15);
        button.setAllCaps(false);
        button.setTypeface(Typeface.create("sans", Typeface.BOLD));
        button.setGravity(Gravity.CENTER);
        button.setMinHeight(dp(52));
        button.setPadding(dp(18), 0, dp(18), 0);
        int background = filled ? primary : surface;
        int foreground = filled ? onPrimary : text;
        GradientDrawable shape = outlined(background, filled ? primary : outline, 14);
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(withAlpha(foreground, 30)), shape, round(Color.WHITE, 14)));
        button.setTextColor(foreground);
        return button;
    }

    private GradientDrawable round(int color, int radius) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(radius));
        return drawable;
    }

    private GradientDrawable outlined(int color, int stroke, int radius) {
        GradientDrawable drawable = round(color, radius);
        drawable.setStroke(dp(1), stroke);
        return drawable;
    }

    private int withAlpha(int color, int alpha) {
        return Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color));
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private LinearLayout.LayoutParams match() {
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams box(int width, int height) {
        return new LinearLayout.LayoutParams(dp(width), dp(height));
    }

    private LinearLayout.LayoutParams topMargin(LinearLayout.LayoutParams params, int margin) {
        params.topMargin = dp(margin);
        return params;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        worker.shutdownNow();
    }
}
