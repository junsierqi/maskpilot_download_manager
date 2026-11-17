"""Verify Windows report publication and failure state using the actual desktop."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import threading


def load_harness():
    source = Path(__file__).resolve().parents[2] / 'benchmarks/run.py'
    spec = importlib.util.spec_from_file_location('maskpilot_report_fixture', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def atomic_publication(module, root):
    report = root / 'atomic.json'
    module.write_report(report, {'version': 0})
    errors, done = [], threading.Event()

    def publish():
        try:
            module.write_report(report, {'version': 1})
        except BaseException as error:
            errors.append(error)
        finally:
            done.set()

    writer = threading.Thread(target=publish)
    try:
        with report.open(encoding='utf-8') as reader:
            writer.start()
            assert not done.wait(.15), 'Windows reader did not block replacement'
            assert json.load(reader) == {'version': 0}, 'Reader saw a partial/new report'
    finally:
        if writer.ident is not None:
            writer.join(timeout=5)
    assert not writer.is_alive() and not errors, errors
    assert json.loads(report.read_text()) == {'version': 1}
    with report.open(encoding='utf-8') as reader:
        try:
            module.write_report(report, {'version': 2})
        except PermissionError:
            pass
        else:
            raise AssertionError('Persistent report lock was silently ignored')
        assert json.load(reader) == {'version': 1}
    print('PASS atomic reports tolerate transient readers and reject persistent locks', flush=True)


def final_publication_failure(module, host, desktop, root):
    original = module.write_report

    def fault(path, value):
        if value.get('status') == 'finishing':
            raise OSError('fixture-final-report-publication-failure')
        original(path, value)

    report = root / 'result.json'
    options = argparse.Namespace(
        desktop=desktop, native_host=host, duration=30, report=report,
        qt_platform='offscreen', keep_artifacts=False, stop_file=None,
        media_parser=None, media_muxer=None, media_fixtures=None, media_every=10)
    module.write_report = fault
    try:
        result = module.run(options)
    finally:
        module.write_report = original
    data = json.loads(report.read_text())
    artifacts = Path(data['artifacts']).resolve()
    try:
        assert result == 1 and data['status'] == 'failed', {
            'result': result, 'status': data['status'], 'failure': data.get('failure')}
        assert data['failure'] == 'OSError: fixture-final-report-publication-failure'
        assert data['processCleaned'] and artifacts.is_dir() and not data.get('artifactsRemoved')
        print('PASS final report publication failure stays failed and retains diagnostics', flush=True)
    finally:
        if (artifacts.parent != Path(tempfile.gettempdir()).resolve() or
                not artifacts.name.startswith('maskpilot-stress-')):
            raise RuntimeError('Refusing unsafe fixture cleanup')
        if artifacts.exists():
            shutil.rmtree(artifacts)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host', type=Path)
    parser.add_argument('desktop', type=Path)
    options = parser.parse_args()
    module = load_harness()
    with tempfile.TemporaryDirectory(prefix='maskpilot-report-test-') as temporary:
        root = Path(temporary).resolve()
        atomic_publication(module, root)
        final_publication_failure(module, options.host.resolve(), options.desktop.resolve(), root)
