<?php
function ccw_to_dasd($dev) {
    $dasd = "";
    $devcmd = "sudo -u root /sbin/lsdasd " . $dev . " 2>&1";
    exec ($devcmd, $devcmd_output);
    foreach ($devcmd_output as $devcmd_line) {
	if (preg_match("/dasd[a-z]*/", $devcmd_line, $matches)) {
	    $dasd = $matches[0];
	}
    }
    return $dasd;
}

$action = "";
if (isset($_GET['ccw'])) {
    $ccwdev = $_GET['ccw'];
}
if (isset($_GET['action'])) {
    $action = $_GET['action'];
}

if (isset($ccwdev)) {
    $err = 0;
    if (!strncmp($action, "offline", 7)) {
	$cmd = "sudo -u root /sbin/chccwdev -d " . $ccwdev;
	exec ($cmd, $output, $err);
    } else if (!strncmp($action, "online", 6)) {
	$cmd = "sudo -u root /sbin/chccwdev -e " . $ccwdev;
	exec ($cmd, $output, $err);
    } else if (!strncmp($action, "Reserve", 7)) {
	$dasddev = ccw_to_dasd($ccwdev);
	if (strlen($dasddev)) {
	    $cmd = "sudo -u root /sbin/tunedasd -S /dev/" . $dasddev;
	    exec ($cmd, $output, $err);
	} else {
	    $output[0] = "DASD " . $ccwdev . " not online";
	    $err = 1;
	}
    } else if (!strncmp($action, "Release", 7)) {
	$dasddev = ccw_to_dasd($ccwdev);
	if (strlen($dasddev)) {
	    $cmd = "sudo -u root /sbin/tunedasd -L /dev/" . $dasddev;
	    exec ($cmd, $output, $err);
	} else {
	    $output[0] = "DASD " . $ccwdev . " not online";
	    $err = 1;
	}
    } else {
	if (strlen($action)) {
	    header("HTTP/1.1 404 invalid action");
	} else {
	    header('HTTP/1.1 404 no action specified');
	}
	exit(0);
    }
    if ($err == 1) {
	header("HTTP/1.1 403 action " . $action . " failed, " . $output[0]);
    } else {
	header("HTTP/1.1 303 action " . $action . " ok.");
	Header('Location: '.$_SERVER['PHP_SELF']);
    }
    exit(0);
}

echo "<html>\n<head>\n";
echo "<title>DASD Admin</title>\n</head>\n<body>\n";

exec ("sudo -u root /sbin/vmcp q userid", $output);
$elems = preg_split("/[\s]+/", $output[0]);
if (isset($elems[0])) {
    $userid = $elems[0];
} else {
    $userid = "LINUX000";
}

print ("<h2>Available DASD channels on " . $userid . "</h2>\n");
$output = "";
exec ('lscss', $output);
echo "<table>\n";
echo "<tr><th>Device</th><th>DevType</th><th>CHPIDs</th>";
echo "<th>active</th><th>Node</th><th>reserved</th></tr>\n";
foreach ($output as $line) {
    $elems = preg_split("/[\s,]+/", $line);
    if (!isset($elems[0]))
	continue;
    if (!strncmp($elems[0], "----", 4))
	continue;
    if (!strncmp($elems[0], "Device", 6))
	continue;
    if (preg_match('/0\.0\.01(5|9)[0-9a-f]/', $elems[0]))
	continue;
    if (strncmp($elems[3], "3990/e9", 7))
	continue;
    echo '<tr>';
    echo '<td>';
    print ($elems[0]);
    echo '</td><td>';
    print ($elems[2]);
    echo '</td><td>';
    $href = $_SERVER['PHP_SELF'] . "?ccw=" . $elems[0] ;
    if (!strncmp($elems[4], "yes", 3)) {
	$new_status = "offline";
	$text = "yes";
	$chpid_num1 = $elems[8];
	$chpid_num2 = $elems[9];
    } else {
	$new_status = "online";
	$text = "no";
	$chpid_num1 = $elems[7];
	$chpid_num2 = $elems[8];
    }
    $chpids = str_split($chpid_num1, 2);
    foreach ($chpids as $chpid) {
	if ($chpid != 0) {
	    print ($chpid . " ");
	}
    }
    $chpids = str_split($chpid_num2, 2);
    foreach ($chpids as $chpid) {
	if ($chpid != 0) {
	    print ($chpid . " ");
	}
    }
    echo '</td>';
    $link = "\"" . $href . "&action=" . $new_status . "\" title=\"Set device " . $new_status . "\"";
    echo '<td>';
    print ("<a href=" . $link . ">" . $text . "</a>");
    echo '</td>';
    echo '<td>';
    $devnode = ccw_to_dasd($elems[0]);
    print ($devnode);
    echo '</td><td>';
    if (strlen($devnode)) {
	$dasdcmd_output = "";
	$dasdcmd = "sudo -u root /sbin/tunedasd -Q /dev/" . $devnode . " 2>&1";
	exec ($dasdcmd, $dasdcmd_output);
	$status = $dasdcmd_output[0];
	if (!strncmp($status, "none", 4)) {
	    $new_status = "Reserve";
	} else if (!strncmp($status, "reserved", 8)) {
	    $new_status = "Release";
	} else {
	    $new_status = "";
	}
	if (strlen($new_status)) {
	    $href = $_SERVER['PHP_SELF'] . "?ccw=" . $elems[0] . "&action=" . $new_status ;
	    $link = "\"" . $href . "\" title=\"" . $new_status . " device\"";
	    print ("<a href=" . $link . ">" . $status . "</a>");
	} else {
	    print ($status);
	}
    }
    echo "</td></tr>\n";
}
echo "</table>\n";
echo "</body>\n</html>\n";
?>
