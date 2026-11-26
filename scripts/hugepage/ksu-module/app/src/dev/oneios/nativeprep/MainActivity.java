package dev.oneios.nativeprep;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.Icon;
import android.graphics.drawable.RippleDrawable;
import android.os.Bundle;
import android.text.method.ScrollingMovementMethod;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private static final String ACTION_PREPARE = "dev.oneios.nativeprep.PREPARE";
    private static final String ACTION_PIN = "dev.oneios.nativeprep.PIN";
    private static final String MODULE_COMMAND = "/data/adb/modules/oneios_native_on_demand/system/bin/oneios-native";
    private static final String COMMAND_PICKER =
            "if [ -x /system/bin/oneios-native ]; then C=/system/bin/oneios-native; " +
            "elif [ -r " + MODULE_COMMAND + " ]; then C='/system/bin/sh " + MODULE_COMMAND + "'; " +
            "else echo 'OneiOS KernelSU module is not installed yet.'; exit 127; fi; exec $C ";

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private TextView statusText;
    private TextView stateLabel;
    private Button prepareButton;
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
        setContentView(buildContent());
        runCommand("status", "Checking host support…");
        if (ACTION_PREPARE.equals(getIntent().getAction())) {
            statusText.postDelayed(this::confirmPrepare, 350);
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
        scroll.addView(root, matchWrap());

        LinearLayout mark = new LinearLayout(this);
        mark.setOrientation(LinearLayout.HORIZONTAL);
        mark.setGravity(Gravity.CENTER_VERTICAL);
        TextView icon = text("M", 18, Typeface.BOLD, onPrimary);
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

        prepareButton = button("Prepare 4 GiB", true);
        prepareButton.setOnClickListener(v -> confirmPrepare());
        root.addView(prepareButton, topMargin(match(), 18));

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
                "No startup service, framework stop, display command, reboot, or automatic VM launch is installed. Module and kernel mismatches are refused before loading.",
                14, Typeface.NORMAL, secondary);
        safetyCopy.setLineSpacing(0, 1.14f);
        safety.addView(safetyTitle);
        safety.addView(safetyCopy, topMargin(matchWrap(), 7));
        root.addView(safety, topMargin(matchWrap(), 24));

        shortcutButton = button("Add Prepare shortcut to Home", false);
        shortcutButton.setOnClickListener(v -> requestShortcut());
        root.addView(shortcutButton, topMargin(match(), 16));

        TextView footer = text("Galaxy S25 Ultra · on-demand module", 12, Typeface.NORMAL, secondary);
        footer.setGravity(Gravity.CENTER);
        root.addView(footer, topMargin(match(), 28));
        return scroll;
    }

    private void confirmPrepare() {
        new AlertDialog.Builder(this)
                .setTitle("Prepare memory for OneiOS?")
                .setMessage("This loads the reviewed support modules and requests 2,032 huge pages. The screen and One UI stay running. No VM starts. Continue?")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Prepare", (dialog, which) -> runCommand("prepare 4096", "Preparing 4 GiB pool…"))
                .show();
    }

    private void confirmRelease() {
        new AlertDialog.Builder(this)
                .setTitle("Release reserved memory?")
                .setMessage("Release is refused if a VM or shared parcel is active. It never force-removes a module.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Release", (dialog, which) -> runCommand("release", "Releasing…"))
                .show();
    }

    private void runCommand(String args, String pending) {
        setBusy(true);
        stateLabel.setText(pending);
        stateLabel.setTextColor(warning);
        statusText.setText("KernelSU may ask you to grant this app root access.\n\n" + pending);
        worker.execute(() -> {
            StringBuilder output = new StringBuilder();
            int code = -1;
            try {
                Process process = new ProcessBuilder("su", "-c", COMMAND_PICKER + args)
                        .redirectErrorStream(true)
                        .start();
                try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()))) {
                    String line;
                    while ((line = reader.readLine()) != null) output.append(line).append('\n');
                }
                code = process.waitFor();
            } catch (Exception e) {
                output.append("Unable to run root command: ").append(e.getMessage()).append('\n');
            }
            final int result = code;
            final String message = output.length() == 0 ? "No output returned." : output.toString().trim();
            runOnUiThread(() -> {
                statusText.setText(message);
                if (result == 0) {
                    stateLabel.setText(args.startsWith("prepare") ? "Ready" : args.equals("release") ? "Released" : "Status updated");
                    stateLabel.setTextColor(success);
                } else {
                    stateLabel.setText(result == 127 ? "Module unavailable" : "Action refused safely");
                    stateLabel.setTextColor(error);
                }
                setBusy(false);
            });
        });
    }

    private void requestShortcut() {
        ShortcutManager manager = getSystemService(ShortcutManager.class);
        if (manager == null || !manager.isRequestPinShortcutSupported()) {
            stateLabel.setText("Launcher does not support pinning");
            stateLabel.setTextColor(error);
            return;
        }
        Intent intent = new Intent(this, MainActivity.class)
                .setAction(ACTION_PREPARE)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        ShortcutInfo shortcut = new ShortcutInfo.Builder(this, "prepare_vm_memory")
                .setShortLabel(getString(R.string.shortcut_prepare))
                .setLongLabel("Prepare OneiOS VM memory")
                .setIcon(Icon.createWithResource(this, R.drawable.ic_launcher))
                .setIntent(intent)
                .build();
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
        releaseButton.setEnabled(!busy);
        refreshButton.setEnabled(!busy);
        shortcutButton.setEnabled(!busy);
        float alpha = busy ? 0.55f : 1f;
        prepareButton.setAlpha(alpha);
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
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
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
